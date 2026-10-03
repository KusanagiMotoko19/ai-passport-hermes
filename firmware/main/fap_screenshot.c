/* SPDX-License-Identifier: Apache-2.0 */
/*
 * FAP_SCREENSHOT_V1 —— 串口截屏协议实现（零帧缓冲版）。
 *
 * 为什么不用"静态/动态整屏缓冲"：
 *   ESP32-C3 只有约 400KB SRAM，本固件带 BLE + 完整 UI，编译期静态预留
 *   153600 字节会直接链接失败（dram0_0_seg overflow）；改成运行期
 *   heap_caps_aligned_alloc 也拿不到连续块，且挤压 BLE 导致
 *   "BLE_INIT: Malloc failed" / 连接后立即断开。
 *
 * 改为"抓帧"路线：临时接管 LVGL 的 flush 回调，把渲染出的每一块像素
 * 直接转发给主机，**不申请任何帧缓冲**。渲染仍由 LVGL 任务完成，本任务
 * 只负责读命令和等块流结束，所以栈可以很小。
 *
 * 协议（主机侧见 bridge/fap_screenshot.py）：
 *   -> "FAP_SCREENSHOT_V1\n"
 *   <- "FAP_SCREENSHOT_V1 <w> <h> RGB565LE <总字节>\n"
 *   <- "B <x> <y> <w> <h> <字节>\n" + 紧密 RGB565LE 像素     （每块，可多块）
 *   <- "FAP_END\n"
 *
 * 其余照文档的坑：先装 USB-Serial/JTAG 驱动、读循环不忙等、优先级压在
 * LVGL(4) 之下、子串滑窗匹配不依赖换行、二进制窗口静默日志、失败静默。
 */
#include "fap_screenshot.h"

#include <stdio.h>
#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "bsp_display.h"

static const char *TAG = "fap_shot";

#define FAP_CMD             "FAP_SCREENSHOT_V1"
#define FAP_CMD_LEN         (sizeof(FAP_CMD) - 1U)
#define FAP_RX_CHUNK        32U
#define FAP_TX_CHUNK        256U
#define FAP_IDLE_MS         200    /* 读不到就退避，绝不忙等 */
#define FAP_TASK_PRIO       3      /* LVGL 为 4，必须更低 */
#define FAP_TASK_STACK      2560U  /* 只读串口 + 等块流；渲染不在本任务栈上 */
#define FAP_GRAB_TIMEOUT_MS 4000U

static char   s_win[FAP_CMD_LEN];
static size_t s_win_len;

/* 抓帧状态：在截屏任务与 LVGL 任务之间共享 */
static lv_display_flush_cb_t s_orig_flush;
static volatile bool     s_grabbing;
static volatile uint32_t s_last_block_ms;
static volatile uint32_t s_grab_rows;   /* 已转发的高度累计 */
static volatile uint32_t s_screen_h;    /* 目标高度（整屏） */

static uint32_t fap_now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

/* 诊断握手：直写 USB-Serial/JTAG（日志路由由 console 配置决定，这里绕开它） */
static void fap_note(const char *msg)
{
    if (usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_write_bytes(msg, strlen(msg), pdMS_TO_TICKS(200));
    }
}

static void fap_send(const uint8_t *data, size_t len)
{
    size_t sent = 0U;
    while (sent < len) {
        size_t chunk = len - sent;
        if (chunk > FAP_TX_CHUNK) {
            chunk = FAP_TX_CHUNK;
        }
        size_t n = usb_serial_jtag_write_bytes(data + sent, chunk, pdMS_TO_TICKS(1000));
        if (n == 0U) {
            return;   /* 主机拔线/缓冲满：放弃本次，任务继续存活 */
        }
        sent += n;
    }
}

/* 接管期间的 flush：转发像素，再交原回调让屏幕照常刷新。
 * 注意本函数运行在 LVGL 任务的上下文。 */
static void fap_grab_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    if (s_grabbing && (area != NULL) && (px_map != NULL)) {
        uint32_t w = (uint32_t)(area->x2 - area->x1 + 1);
        uint32_t h = (uint32_t)(area->y2 - area->y1 + 1);
        uint32_t bytes = w * h * 2U;
        char bh[48];
        int n = snprintf(bh, sizeof bh, "B %d %d %u %u %u\n",
                         (int)area->x1, (int)area->y1, (unsigned)w, (unsigned)h,
                         (unsigned)bytes);
        if (n > 0) {
            fap_send((const uint8_t *)bh, (size_t)n);
            fap_send(px_map, bytes);
        }
        s_last_block_ms = fap_now_ms();
        s_grab_rows += h;
        if (s_grab_rows >= s_screen_h) {
            /* 整屏已齐：立即停止转发。不清回调（在渲染上下文里不安全），
             * 由截屏任务看到 s_grabbing 变假后统一恢复。 */
            s_grabbing = false;
        }
    }
    if (s_orig_flush != NULL) {
        s_orig_flush(disp, area, px_map);
    }
}

static void fap_capture(void)
{
    if (!usb_serial_jtag_is_driver_installed()) {
        return;
    }
    if (!bsp_lvgl_lock(2000)) {
        fap_note("FAP_ERR lock\n");
        return;
    }

    lv_display_t *disp = lv_display_get_default();
    if (disp == NULL) {
        bsp_lvgl_unlock();
        return;
    }
    uint32_t w = lv_display_get_horizontal_resolution(disp);
    uint32_t h = lv_display_get_vertical_resolution(disp);

    char header[64];
    int hn = snprintf(header, sizeof header, "%s %u %u RGB565LE %u\n",
                      FAP_CMD, (unsigned)w, (unsigned)h, (unsigned)(w * h * 2U));
    if (hn <= 0) {
        bsp_lvgl_unlock();
        return;
    }

    /* 二进制窗口：日志与应答共用同一条 CDC 流，必须静默 */
    esp_log_level_set("*", ESP_LOG_NONE);
    fap_send((const uint8_t *)header, (size_t)hn);

    /* 换 flush 回调 + 标记整屏脏，然后立刻放锁，让 LVGL 任务自己去渲染 */
    s_grab_rows = 0U;
    s_screen_h = h;
    s_orig_flush = lv_display_get_flush_cb(disp);
    lv_display_set_flush_cb(disp, fap_grab_flush);
    s_last_block_ms = fap_now_ms();
    s_grabbing = true;
    lv_obj_invalidate(lv_screen_active());
    bsp_lvgl_unlock();

    /* 等整屏凑齐（flush 回调达标后置 s_grabbing=false），兜底超时 */
    uint32_t t0 = fap_now_ms();
    while (s_grabbing && ((fap_now_ms() - t0) < FAP_GRAB_TIMEOUT_MS)) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    if (bsp_lvgl_lock(2000)) {
        s_grabbing = false;
        if (s_orig_flush != NULL) {
            lv_display_set_flush_cb(disp, s_orig_flush);
        }
        s_orig_flush = NULL;
        bsp_lvgl_unlock();
    } else {
        s_grabbing = false;
    }

    fap_send((const uint8_t *)"FAP_END\n", 8U);
    esp_log_level_set("*", ESP_LOG_INFO);
    fap_note("FAP_DONE\n");
}

static void fap_task(void *arg)
{
    (void)arg;

    /* 不启动 REPL 的固件里没人装过 USB-Serial/JTAG 驱动：必须显式安装，
     * 否则读会解引用空驱动对象（表现为反复崩溃重启 / "屏幕一直闪"）。 */
    /* 缓冲尺寸有下限：rx=64 会让 usb_serial_jtag_driver_install 直接失败，
     * 之后 is_driver_installed() 恒为假——连诊断握手都发不出去，
     * 而且没人读口会让主机的 write 阻塞。256/1024 是实测可用的最小组合。 */
    usb_serial_jtag_driver_config_t cfg = {
        .rx_buffer_size = 256,
        .tx_buffer_size = 1024,
    };
    esp_err_t ierr = usb_serial_jtag_driver_install(&cfg);
    if (ierr == ESP_OK) {
        usb_serial_jtag_vfs_use_driver();
    }
    {
        char m[80];
        snprintf(m, sizeof m, "FAP_BOOT drv=%s vfs=%d\n",
                 esp_err_to_name(ierr), (int)usb_serial_jtag_is_driver_installed());
        fap_note(m);
    }

    uint8_t buf[FAP_RX_CHUNK];
    for (;;) {
        if (!usb_serial_jtag_is_driver_installed()) {
            vTaskDelay(pdMS_TO_TICKS(FAP_IDLE_MS));
            continue;
        }

        int n = usb_serial_jtag_read_bytes(buf, sizeof buf, pdMS_TO_TICKS(100));
        if (n <= 0) {
            vTaskDelay(pdMS_TO_TICKS(FAP_IDLE_MS));   /* 绝不忙等空转 */
            continue;
        }

        for (int i = 0; i < n; ++i) {
            const char c = (char)buf[i];
            if ((c == '\r') || (c == '\n') || (c == '\0')) {
                s_win_len = 0U;
                continue;
            }
            if (s_win_len < FAP_CMD_LEN) {
                s_win[s_win_len++] = c;
            } else {
                memmove(s_win, s_win + 1, FAP_CMD_LEN - 1U);
                s_win[FAP_CMD_LEN - 1U] = c;
            }
            if ((s_win_len == FAP_CMD_LEN) &&
                (memcmp(s_win, FAP_CMD, FAP_CMD_LEN) == 0)) {
                s_win_len = 0U;   /* 命中后清空，防残留字节连续误触发 */
                fap_note("FAP_CMD\n");
                fap_capture();
            }
        }
    }
}

esp_err_t fap_screenshot_start(void)
{
    BaseType_t ok = xTaskCreate(fap_task, "fap_shot", FAP_TASK_STACK, NULL,
                                FAP_TASK_PRIO, NULL);
    return (ok == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}
