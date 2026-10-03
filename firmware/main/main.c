#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "cJSON.h"      /* history_add 命令解析（桥补推历史，见 buddy_try_history_command） */

#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "buddy_app_logic.h"
#include "buddy_ble.h"
#include "buddy_adpcm.h"
#include "buddy_orchestrator.h"
#include "buddy_protocol.h"
#include "buddy_settings.h"
#include "buddy_state.h"
#include "buddy_ui.h"
#include "fap_screenshot.h"

#define BUDDY_CRITICAL_QUEUE_DEPTH 1U
#define BUDDY_BUTTON_QUEUE_DEPTH 4U
#define BUDDY_RX_NORMAL_QUEUE_DEPTH 1U
#define BUDDY_RX_SLOT_COUNT 6U
/* Priority may consume the entire shared pool after evicting the normal slot. */
#define BUDDY_RX_PRIORITY_QUEUE_DEPTH BUDDY_RX_SLOT_COUNT
#define BUDDY_APP_STACK_SIZE 12288U
#define BUDDY_APP_PRIORITY 5U
#define BUDDY_APP_TICK_MS 100U
#define BUDDY_BATTERY_SAMPLE_MS 10000ULL
#define BUDDY_SETTINGS_SERVICE_MS 1000ULL

typedef enum {
    BUDDY_CONTROL_KEY,
    BUDDY_CONTROL_BLE_CONNECTED,
    BUDDY_CONTROL_BLE_DISCONNECTED,
    BUDDY_CONTROL_BLE_PASSKEY,
    BUDDY_CONTROL_BLE_ENCRYPTION,
    BUDDY_CONTROL_BOND_DELETE_RESULT,
} buddy_control_type_t;

typedef struct {
    buddy_control_type_t type;
    union {
        struct {
            bsp_btn_t button;
            bsp_btn_ev_t event;
            uint32_t view_generation;
            buddy_page_t page;
            buddy_confirmation_t confirmation;
            buddy_settings_item_t settings_selection;
            buddy_menu_item_t menu_selection;
            buddy_reset_item_t reset_selection;
            bool menu_open;
            bool reset_open;
            bool approval_visible;
            bool passkey_visible;
            bool ble_enabled;
            uint32_t sensitive_connection_generation;
            char prompt_id[BUDDY_PROMPT_ID_MAX];
        } key;
        struct {
            uint32_t passkey;
            uint32_t connection_generation;
            int status;
            bool secure;
            bool success;
        } ble;
    } data;
} buddy_control_event_t;

typedef struct {
    uint32_t generation;
    buddy_page_t page;
    buddy_confirmation_t confirmation;
    buddy_settings_item_t settings_selection;
    buddy_menu_item_t menu_selection;
    buddy_reset_item_t reset_selection;
    bool menu_open;
    bool reset_open;
    bool approval_visible;
    bool passkey_visible;
    bool ble_enabled;
    uint32_t sensitive_connection_generation;
    char prompt_id[BUDDY_PROMPT_ID_MAX];
    char prompt_tool[BUDDY_TOOL_MAX];
    char prompt_hint[BUDDY_HINT_MAX];
} buddy_rendered_view_t;

typedef struct {
    char data[BUDDY_JSON_LINE_MAX + 1U];
    size_t length;
    uint32_t connection_generation;
    bool in_use;
} buddy_rx_slot_t;

static const char *const TAG = "buddy_app";
static QueueHandle_t s_link_queue;
static QueueHandle_t s_passkey_queue;
static QueueHandle_t s_security_queue;
static QueueHandle_t s_bond_queue;
static QueueHandle_t s_button_queue;
static QueueHandle_t s_rx_normal_queue;
static QueueHandle_t s_rx_priority_queue;
static TaskHandle_t s_app_task_handle;
static buddy_rx_slot_t s_rx_slots[BUDDY_RX_SLOT_COUNT];
static portMUX_TYPE s_rx_pool_lock = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_view_lock = portMUX_INITIALIZER_UNLOCKED;
static buddy_rendered_view_t s_rendered_view;
static buddy_settings_snapshot_t s_initial_settings;
static bool s_initial_battery_available;
static atomic_bool s_ble_initialized;
static atomic_bool s_app_ready;
static atomic_uint s_control_coalesced;
static atomic_uint s_button_dropped;
static atomic_uint s_rx_normal_coalesced;
static atomic_uint s_rx_priority_evicted;
static atomic_uint s_rx_dropped;

static uint64_t buddy_now_ms(void)
{
    return (uint64_t)esp_timer_get_time() / 1000ULL;
}

static void buddy_copy_text(char *destination, size_t destination_size, const char *source)
{
    size_t length = 0;

    if (destination_size == 0U) {
        return;
    }
    if (source != NULL) {
        while (length + 1U < destination_size && source[length] != '\0') {
            ++length;
        }
        memcpy(destination, source, length);
    }
    destination[length] = '\0';
}

static void buddy_default_name(char name[BUDDY_NAME_MAX])
{
    uint8_t mac[6];

    if (esp_read_mac(mac, ESP_MAC_BT) == ESP_OK) {
        (void)snprintf(name, BUDDY_NAME_MAX, "Hermes-%02X%02X%02X", mac[3], mac[4], mac[5]);
    } else {
        buddy_copy_text(name, BUDDY_NAME_MAX, "Hermes-Buddy");
    }
}

static buddy_rx_slot_t *buddy_rx_slot_acquire(void)
{
    buddy_rx_slot_t *slot = NULL;
    unsigned index;

    taskENTER_CRITICAL(&s_rx_pool_lock);
    for (index = 0; index < BUDDY_RX_SLOT_COUNT; ++index) {
        if (!s_rx_slots[index].in_use) {
            s_rx_slots[index].in_use = true;
            slot = &s_rx_slots[index];
            break;
        }
    }
    taskEXIT_CRITICAL(&s_rx_pool_lock);
    return slot;
}

static void buddy_rx_slot_release(buddy_rx_slot_t *slot)
{
    if (slot == NULL) {
        return;
    }
    taskENTER_CRITICAL(&s_rx_pool_lock);
    slot->length = 0;
    slot->connection_generation = 0;
    slot->data[0] = '\0';
    slot->in_use = false;
    taskEXIT_CRITICAL(&s_rx_pool_lock);
}

static void buddy_count(atomic_uint *counter)
{
    (void)atomic_fetch_add_explicit(counter, 1U, memory_order_relaxed);
}

static void buddy_notify_app(void)
{
    if (s_app_task_handle != NULL &&
        atomic_load_explicit(&s_app_ready, memory_order_acquire)) {
        xTaskNotifyGive(s_app_task_handle);
    }
}

static void buddy_queue_critical(QueueHandle_t queue,
                                 const buddy_control_event_t *event)
{
    BaseType_t queued;

    if (queue == NULL || event == NULL) {
        return;
    }
    if (uxQueueMessagesWaiting(queue) != 0U) {
        buddy_count(&s_control_coalesced);
    }
    queued = xQueueOverwrite(queue, event);
    configASSERT(queued == pdPASS);
    (void)queued;
    buddy_notify_app();
}

static bool buddy_rx_evict(QueueHandle_t queue)
{
    buddy_rx_slot_t *evicted = NULL;

    if (xQueueReceive(queue, &evicted, 0) != pdTRUE || evicted == NULL) {
        return false;
    }
    buddy_rx_slot_release(evicted);
    return true;
}

static void buddy_apply_rx_retry_counts(const buddy_app_rx_retry_state_t *retry)
{
    if (retry->normal_evictions != 0U) {
        (void)atomic_fetch_add_explicit(&s_rx_normal_coalesced,
                                        retry->normal_evictions,
                                        memory_order_relaxed);
    }
    if (retry->priority_evictions != 0U) {
        (void)atomic_fetch_add_explicit(&s_rx_priority_evicted,
                                        retry->priority_evictions,
                                        memory_order_relaxed);
    }
}

static void buddy_queue_rx_line(const buddy_ble_event_t *event)
{
    if (event->data.rx_line.data == NULL || event->data.rx_line.length == 0U) {
        buddy_count(&s_rx_dropped);
        return;
    }
    buddy_app_rx_class_t classification =
        buddy_app_classify_rx(event->data.rx_line.data, event->data.rx_line.length);
    QueueHandle_t target = classification == BUDDY_APP_RX_NORMAL_HEARTBEAT
                               ? s_rx_normal_queue
                               : s_rx_priority_queue;
    buddy_app_rx_retry_state_t retry;
    buddy_rx_slot_t *slot = buddy_rx_slot_acquire();
    buddy_app_rx_overflow_action_t overflow;

    buddy_app_rx_retry_init(&retry, classification);
    for (;;) {
        bool normal_pending = uxQueueMessagesWaiting(s_rx_normal_queue) != 0U;
        UBaseType_t priority_count = uxQueueMessagesWaiting(s_rx_priority_queue);
        bool evicted;

        overflow = buddy_app_rx_retry_next(
            &retry, slot != NULL, normal_pending, priority_count != 0U,
            priority_count >= BUDDY_RX_PRIORITY_QUEUE_DEPTH);
        if (overflow == BUDDY_APP_RX_ENQUEUE) {
            break;
        }
        if (overflow == BUDDY_APP_RX_DROP) {
            if (slot != NULL) {
                buddy_rx_slot_release(slot);
            }
            buddy_apply_rx_retry_counts(&retry);
            buddy_count(&s_rx_dropped);
            return;
        }
        evicted = buddy_rx_evict(
            overflow == BUDDY_APP_RX_REPLACE_NORMAL ? s_rx_normal_queue
                                                    : s_rx_priority_queue);
        buddy_app_rx_retry_record_eviction(&retry, overflow, evicted);
        if (slot == NULL) {
            slot = buddy_rx_slot_acquire();
        }
    }
    memcpy(slot->data, event->data.rx_line.data, event->data.rx_line.length);
    slot->data[event->data.rx_line.length] = '\0';
    slot->length = event->data.rx_line.length;
    slot->connection_generation = event->data.rx_line.connection_generation;
    if (xQueueSend(target, &slot, 0) == pdTRUE) {
        buddy_apply_rx_retry_counts(&retry);
        buddy_notify_app();
        return;
    }

    /* The app may race the initial snapshot; retain the newest item once. */
    if (classification == BUDDY_APP_RX_NORMAL_HEARTBEAT) {
        overflow = BUDDY_APP_RX_REPLACE_NORMAL;
        buddy_app_rx_retry_record_eviction(
            &retry, overflow, buddy_rx_evict(s_rx_normal_queue));
    } else {
        overflow = BUDDY_APP_RX_REPLACE_OLDEST_PRIORITY;
        buddy_app_rx_retry_record_eviction(
            &retry, overflow, buddy_rx_evict(s_rx_priority_queue));
    }
    if (xQueueSend(target, &slot, 0) != pdTRUE) {
        buddy_rx_slot_release(slot);
        buddy_apply_rx_retry_counts(&retry);
        buddy_count(&s_rx_dropped);
    } else {
        buddy_apply_rx_retry_counts(&retry);
        buddy_notify_app();
    }
}

/* OK 键单击（BLE 未连接时）= 切换本地采集开关；长按仍走菜单流程（见 on_key / buddy_mic_task）。 */
static volatile bool s_mic_rec_toggle;
/* 采集开始时刻；0 = 未在采集。屏幕据此显示"正在录音 + 已录秒数"。 */
static volatile TickType_t s_mic_rec_start;

/* 已采集秒数；未在采集时返回 -1。 */
static int buddy_mic_rec_seconds(void)
{
    TickType_t t0 = s_mic_rec_start;
    if (t0 == 0) return -1;
    return (int)((xTaskGetTickCount() - t0) / (TickType_t)pdMS_TO_TICKS(1000));
}

static void on_key(bsp_btn_t button, bsp_btn_ev_t event, void *context)
{
    buddy_control_event_t control = {0};

    (void)context;
    /* 双击（10/04 科长定）：**下键单击 = 往上翻**（翻到更旧的一条），
     * **下键双击 = 往下翻**（翻回较新的一条）。一个下键就能把 10 条记录上下翻完。
     * ⚠ 不再把双击改写成「上键单击」——那条路会把 up/click 一并发给桥，桥再往
     *   Hermes 注入一次上翻，凭空多动一次。现在双击用自己的事件类型，只在本机生效
     *   （buddy_report_key 只认 CLICK/LONG，DOUBLE 天然不上报）。 */
    if (event == BSP_BTN_DOUBLE && button != BSP_BTN_DOWN) {
        return;                 /* 其余键的双击仍然丢弃 */
    }
    if ((event != BSP_BTN_CLICK && event != BSP_BTN_LONG && event != BSP_BTN_DOUBLE) ||
        s_button_queue == NULL) {
        return;
    }
    /* 单击 OK：不再在设备端本地开录，直接落到下面的上报路径 ——
     * 主机（桥）收到 ok.click 后才发 mic_start 驱动上行与转写。
     * ⚠ 这里曾经是 s_mic_rec_toggle = true; buddy_notify_app(); return;（本地开录 + 不上报），
     * 后果有两条：① 桥永远收不到 {"cmd":"key","k":"ok","ev":"click"}，语音输入整条链路形同虚设；
     * ② 设备端本地开关与主机开关两套状态机打架，第二下按 OK 停不掉上行。
     * 转圈指示不受影响：buddy_ui 对「本地采集」和「主机 mic_start」两种状态都亮。 */
    if (button == BSP_BTN_OK && event == BSP_BTN_CLICK) {
        buddy_notify_app();
    }
    /* ⚠ 上下键短按**不再**做正文滚动（2026-10-03 科长定）：设备屏现在只显示
     * 「一屏装得下」的摘要（桥的 extract_card 优先取回复里的要点块），压根不需要翻页。
     * 两个键因此空出来 —— 让它们落到下面的上报路径，由桥的 keymap 决定做什么
     * （默认不绑动作，免得空按键被误触）。长按语义不变：上=新建会话、下=确定审批。 */
    control.type = BUDDY_CONTROL_KEY;
    control.data.key.button = button;
    control.data.key.event = event;
    taskENTER_CRITICAL(&s_view_lock);
    control.data.key.view_generation = s_rendered_view.generation;
    control.data.key.page = s_rendered_view.page;
    control.data.key.confirmation = s_rendered_view.confirmation;
    control.data.key.settings_selection = s_rendered_view.settings_selection;
    control.data.key.menu_selection = s_rendered_view.menu_selection;
    control.data.key.reset_selection = s_rendered_view.reset_selection;
    control.data.key.menu_open = s_rendered_view.menu_open;
    control.data.key.reset_open = s_rendered_view.reset_open;
    control.data.key.approval_visible = s_rendered_view.approval_visible;
    control.data.key.passkey_visible = s_rendered_view.passkey_visible;
    control.data.key.ble_enabled = s_rendered_view.ble_enabled;
    control.data.key.sensitive_connection_generation =
        s_rendered_view.sensitive_connection_generation;
    memcpy(control.data.key.prompt_id, s_rendered_view.prompt_id,
           sizeof(control.data.key.prompt_id));
    taskEXIT_CRITICAL(&s_view_lock);
    if (xQueueSend(s_button_queue, &control, 0) != pdTRUE) {
        buddy_count(&s_button_dropped);
    } else {
        buddy_notify_app();
    }
}

static void on_ble_event(const buddy_ble_event_t *event, void *context)
{
    buddy_control_event_t control = {0};

    (void)context;
    if (event == NULL) {
        return;
    }
    if (event->type == BUDDY_BLE_EVENT_RX_LINE) {
        if (event->data.rx_line.length > BUDDY_JSON_LINE_MAX) {
            buddy_count(&s_rx_dropped);
            return;
        }
        buddy_queue_rx_line(event);
        return;
    }

    switch (event->type) {
    case BUDDY_BLE_EVENT_CONNECTED:
        control.type = BUDDY_CONTROL_BLE_CONNECTED;
        control.data.ble.connection_generation =
            event->data.connected.connection_generation;
        buddy_queue_critical(s_link_queue, &control);
        break;
    case BUDDY_BLE_EVENT_DISCONNECTED:
        control.type = BUDDY_CONTROL_BLE_DISCONNECTED;
        control.data.ble.status = event->data.disconnected.reason;
        control.data.ble.connection_generation =
            event->data.disconnected.connection_generation;
        buddy_queue_critical(s_link_queue, &control);
        break;
    case BUDDY_BLE_EVENT_PASSKEY:
        control.type = BUDDY_CONTROL_BLE_PASSKEY;
        control.data.ble.passkey = event->data.passkey.value;
        control.data.ble.connection_generation =
            event->data.passkey.connection_generation;
        buddy_queue_critical(s_passkey_queue, &control);
        break;
    case BUDDY_BLE_EVENT_ENCRYPTION:
        control.type = BUDDY_CONTROL_BLE_ENCRYPTION;
        control.data.ble.status = event->data.encryption.status;
        control.data.ble.connection_generation =
            event->data.encryption.connection_generation;
        control.data.ble.secure = event->data.encryption.status == 0 &&
                                  event->data.encryption.encrypted &&
                                  event->data.encryption.authenticated &&
                                  event->data.encryption.bonded;
        buddy_queue_critical(s_security_queue, &control);
        break;
    case BUDDY_BLE_EVENT_BOND_DELETE_RESULT:
        control.type = BUDDY_CONTROL_BOND_DELETE_RESULT;
        control.data.ble.status = event->data.bond_delete_result.status;
        control.data.ble.success = event->data.bond_delete_result.success;
        buddy_queue_critical(s_bond_queue, &control);
        break;
    case BUDDY_BLE_EVENT_RX_LINE:
        return;
    }
}

static bool buddy_key_matches_rendered_view(const buddy_control_event_t *control)
{
    bool matches;

    taskENTER_CRITICAL(&s_view_lock);
    matches = control->data.key.view_generation != 0U &&
              control->data.key.view_generation == s_rendered_view.generation;
    taskEXIT_CRITICAL(&s_view_lock);
    return matches;
}

static bool buddy_key_matches_state(const buddy_control_event_t *control,
                                    const buddy_state_t *state)
{
    if (control->data.key.passkey_visible) {
        return false;
    }
    if (control->data.key.confirmation != BUDDY_CONFIRM_NONE) {
        return state->confirmation == control->data.key.confirmation &&
               state->confirmation_connection_generation ==
                   control->data.key.sensitive_connection_generation;
    }
    if (control->data.key.approval_visible) {
        return state->confirmation == BUDDY_CONFIRM_NONE && !state->passkey_visible &&
               strcmp(state->prompt.id, control->data.key.prompt_id) == 0 &&
               state->prompt_connection_generation ==
                   control->data.key.sensitive_connection_generation;
    }
    return state->confirmation == BUDDY_CONFIRM_NONE && !state->passkey_visible &&
           state->prompt.id[0] == '\0' && state->page == control->data.key.page &&
           state->menu_open == control->data.key.menu_open &&
           (!state->menu_open || state->menu_selection == control->data.key.menu_selection) &&
           state->reset_open == control->data.key.reset_open &&
           (!state->reset_open || state->reset_selection == control->data.key.reset_selection) &&
           (state->page != BUDDY_PAGE_SETTINGS ||
            (state->settings_selection == control->data.key.settings_selection &&
             (state->settings_selection != BUDDY_SETTINGS_BLE ||
              state->settings.ble_enabled == control->data.key.ble_enabled)));
}

/* --- Hermes 桥扩展:把按键事件上报给主机 ---------------------------------
 * 主机(桌面 Hermes)收到 {"cmd":"key","k":"up|down|ok","ev":"click|long"} 后
 * 翻译成 Hermes 的快捷键注入。未建立加密链路时静默丢弃。
 * 只在 app task 上下文调用:buddy_ble_send 直接走 NimBLE 通知,不要放进
 * 按键回调(定时器任务)。 */
/* --- Hermes 扩展:把 snapshot 里的显示类设置落到硬件 ---------------------
 * 灯效 = 背光呼吸/脉冲(LEDC PWM 调光,不占额外内存;纯 BSP 调用,不依赖 LVGL)。
 * 钟向(屏幕旋转)放在 buddy_ui.c —— 那里已持有 LVGL 锁。 */
/* 息屏冻结标志（10/04 修）：只把背光设 0 是不够的 —— buddy_render() 每帧都会调用
 * 本函数，而默认灯效 led_effect=1（呼吸）会把背光在 10~100% 之间扫，0% 立刻被覆盖，
 * 表现为「点了息屏屏幕不灭」（科长 10/04 实测）。息屏期间这里每帧强制归零，压过灯效；
 * 唤醒（BUDDY_ACTION_DISPLAY_BACKLIGHT）时清除。 */
static bool s_screen_off;

static void buddy_apply_display_settings(const buddy_ui_snapshot_t *snap, uint64_t now_ms)
{
    if (snap == NULL) {
        return;
    }
    if (s_screen_off) {
        bsp_display_backlight(0);
        return;
    }
    /* 灯效:0=关(交回亮度设置) 1=呼吸 2=脉冲 3=常亮 */
    if (snap->led_effect == 0) {
        return;
    }
    {
        uint32_t t = (uint32_t)(now_ms % 2000U);
        uint8_t pct;
        if (snap->led_effect == 1U) {          /* 呼吸:10..100 之间来回 */
            uint32_t half = t < 1000U ? t : 2000U - t;
            pct = (uint8_t)(10U + half * 90U / 1000U);
        } else if (snap->led_effect == 2U) {   /* 脉冲:快闪 */
            pct = (t % 250U) < 125U ? 100U : 15U;
        } else {                                /* 常亮:满亮度 */
            pct = 100U;
        }
        bsp_display_backlight(pct);
    }
}

/* --- Hermes 扩展:音频(提示音 + 语音输入共用) ---------------------------
 * 音频在 app task 内按需开启,不占用常驻 DMA 缓冲。 */
#if 1
static bool s_audio_ready = false;
/* ⚠ 提示音暂关：buddy_beep 跑在 app 任务，语音播报跑在独立的 buddy_audio_task，
 *   两者并发碰 bsp_audio_init/set_format 会互相打断。等播报稳定后再把提示音
 *   并入音频任务统一调度。 */
static bool s_sound_enabled = false;

static bool buddy_audio_ensure(uint32_t hz)
{
    if (!s_audio_ready) {
        if (bsp_audio_init() != ESP_OK) {
            ESP_LOGW(TAG, "audio init failed");
            return false;
        }
        s_audio_ready = true;
        /* 10/03 晚：codec 初始化后按 NVS 里的音量设一次（原来音量是写死的，菜单调了不起作用） */
        bsp_audio_set_volume(s_initial_settings.volume <= 100U ? s_initial_settings.volume
                                                               : 80U);
    }
    /* 每次都调用：同格式秒返回，异格式（提示音 16kHz / 语音 8kHz）自动重开。
     * 只在首次设格式会让 8kHz 语音按 16kHz 播放 —— 语速快一倍、音调发尖。 */
    if (bsp_audio_set_format(hz, 16, 1) != ESP_OK) {
        ESP_LOGW(TAG, "audio format failed");
        return false;
    }
    return true;
}

/* 提示音:方波(蜂鸣感)+ 渐弱包络,最长 100ms @16kHz */
static void buddy_beep(uint16_t hz, uint16_t ms)
{
    enum { BEEP_SAMPLES = 400 };             /* 25ms @16kHz —— 够提示音用,省 DRAM(BLE 吃紧) */
    static int16_t buf[BEEP_SAMPLES];
    uint32_t n;
    uint32_t half;
    uint32_t i;

    if (!s_sound_enabled || hz == 0U || ms == 0U) {
        return;
    }
    n = 16000U * ms / 1000U;
    if (n > BEEP_SAMPLES) {
        n = BEEP_SAMPLES;
    }
    half = 16000U / (2U * hz);
    if (half == 0U) {
        half = 1U;
    }
    for (i = 0; i < n; ++i) {
        int32_t env = 7000 - (int32_t)(i * 5000U / n);
        buf[i] = (int16_t)(((i / half) & 1U) ? env : -env);
    }
    if (!buddy_audio_ensure(16000)) {
        return;
    }
    (void)bsp_audio_write(buf, n * sizeof(int16_t));
}
#endif

/* 收到新的回复正文时响一声（正文变化即视为新消息，用于确认喇叭通路）。 */
static void buddy_notify_new_body(const buddy_ui_snapshot_t *snapshot)
{
    static char last[BUDDY_BODY_MAX];
    if (snapshot == NULL || strcmp(last, snapshot->body) == 0) {
        return;
    }
    strncpy(last, snapshot->body, sizeof(last) - 1U);
    last[sizeof(last) - 1U] = '\0';
    if (snapshot->body[0] != '\0') {
        buddy_beep(2200, 60);
    }
}

/* u-law → 线性 PCM（G.711 标准解码）。 */
static int16_t buddy_ulaw_to_pcm(uint8_t value)
{
    int t;

    value = (uint8_t)~value;
    t = ((value & 0x0FU) << 3) + 0x84;
    t <<= (value >> 4) & 0x07U;
    return (int16_t)((value & 0x80U) ? (0x84 - t) : (t - 0x84));
}

/* 音频播放：独立任务，不挂在 app 任务的 tick 里。
 *
 * ⚠ 挂 app 任务会断续，两条原因：
 *   1) bsp_audio_write() 是阻塞写，排空一轮几十~上百 ms，期间 I2S DMA
 *      (3×120 帧 ≈ 90ms @8kHz 单声道)经常饿着 -> 声音一顿一顿；
 *   2) 同一段时间 BLE 仍按 8 KB/s 灌数据，环形缓冲被写溢出而丢弃
 *      (实测主机送 25344 字节, 设备只落 22947)。
 * 独立任务按 5ms 粒度盯住环形缓冲，供给就均匀了。 */
static bool s_audio_playing = false;
static uint32_t s_audio_bytes = 0;

#define BUDDY_AUDIO_STACK_SIZE 3072U
#define BUDDY_AUDIO_PRIORITY   5U          /* 与 app 任务同级轮转：
                                            * 低于它会被 LVGL 渲染饿着(出声就断)，
                                            * 高于它又会抢 BLE host 的处理时机。 */

static void buddy_audio_task(void *context)
{
    /* ADPCM 每字节承载 2 个采样，所以 PCM 缓冲要开成输入的两倍。 */
    enum { AUDIO_CHUNK = 512, AUDIO_PCM_MAX = AUDIO_CHUNK * 2 };
    static uint8_t raw[AUDIO_CHUNK];
    static int16_t pcm[AUDIO_PCM_MAX];
    static buddy_adpcm_state_t adpcm;
    size_t got;
    size_t i;
    size_t samples;

    (void)context;
    for (;;) {
        if (!s_audio_playing) {
            if (!buddy_ble_audio_receiving()) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            if (!buddy_audio_ensure(buddy_ble_audio_rate())) {
                buddy_ble_audio_abort();
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }
            buddy_adpcm_reset(&adpcm);
            s_audio_playing = true;
            s_audio_bytes = 0U;
            ESP_LOGI(TAG, "音频泵启动 (%lu Hz %s)",
                     (unsigned long)buddy_ble_audio_rate(),
                     buddy_ble_audio_is_adpcm() ? "IMA-ADPCM" : "u-law");
        }

        got = buddy_ble_audio_read(raw, sizeof(raw));
        /* ⚠ 溢出必须在解码之前处理（10/04）：环形缓冲满过一次 = 音频流中间少了若干
         * 字节，而 IMA-ADPCM 是**有状态差分编码** —— 解码器的预测值/步长和编码端从此
         * 错开，继续解码会把预测误差外推成满量程尖峰，也就是用户听到的那声
         * 「突然很巨大 + 毛刺」。这里直接清零解码状态，并**丢掉手里这批已经不可信的
         * 数据**：宁可静音几毫秒，也不要一声爆响。 */
        if (buddy_ble_audio_take_overflow()) {
            ESP_LOGW(TAG, "音频溢出：丢弃 %u 字节并重置 ADPCM 状态（累计 %lu 次）",
                     (unsigned)got, (unsigned long)buddy_ble_audio_overruns());
            buddy_adpcm_reset(&adpcm);
            got = 0U;                      /* 这批数据按「没收到」处理 */
        }
        if (got == 0U) {
            if (!buddy_ble_audio_receiving()) {
                s_audio_playing = false;
                ESP_LOGI(TAG, "音频泵结束，共送 %lu 字节到 I2S",
                         (unsigned long)s_audio_bytes);
            } else {
                vTaskDelay(pdMS_TO_TICKS(5));   /* 等下一批，不忙等 */
            }
            continue;
        }
        if (buddy_ble_audio_is_adpcm()) {
            buddy_adpcm_decode(raw, got, pcm, &adpcm);
            samples = got * 2U;
        } else {
            for (i = 0U; i < got; ++i) {
                pcm[i] = buddy_ulaw_to_pcm(raw[i]);
            }
            samples = got;
        }
        if (bsp_audio_write(pcm, samples * sizeof(int16_t)) != ESP_OK) {
            ESP_LOGW(TAG, "audio write failed");
        }
        s_audio_bytes += (uint32_t)got;
    }
}


/* 麦克风上行：主机命令开启后，把 I2S RX 的 PCM 编成 IMA-ADPCM 经 BLE 通知发回。
 * ⚠ 与播放共用同一个 codec，两者互斥；上行实测速率是这套方案的主要风险点。 */
#define BUDDY_MIC_STACK_SIZE 3072U

/* OK 键的采集开关请求：返回处理后的采集状态。
 * ⚠ 必须在内层采集循环里也调用 —— 只在最外层检查的话，采集一旦开始就死在内层
 * while 里，s_mic_rec_toggle 永远没人看，"再按一次停"就永远不生效。 */
static bool buddy_mic_apply_toggle(bool rec)
{
    if (!s_mic_rec_toggle) return rec;
    s_mic_rec_toggle = false;
    if (buddy_ble_mic_active()) return rec;   /* 主机在用，忽略本地请求 */
    if (rec) {
        s_mic_rec_start = 0;
        ESP_LOGW(TAG, "==== 采集停止 ====");
        return false;
    }
    s_mic_rec_start = xTaskGetTickCount();
    ESP_LOGW(TAG, "==== 采集开始（再按一下 OK 键停止）====");
    return true;
}

/* 本地采集开关（OK 键单击切换；长按 OK 仍是菜单，见 on_key）。
 *   判据只看串口日志里的 "mic seg" 分段统计（由 bsp_audio_read 输出）；
 *   采集期间完全不碰 BLE —— 所以 BLE 配对坏着也能验麦克风。 */
static void buddy_mic_task(void *context)
{
    enum { MIC_SAMPLES = 512 };              /* 每轮 512 采样 ≈ 32ms @16kHz */
    static int16_t pcm[MIC_SAMPLES];
    static uint8_t adpcm[MIC_SAMPLES / 2];
    static buddy_adpcm_state_t enc_state;
    size_t samples;
    bool rec = false;                        /* 本地采集开关 */

    (void)context;
    for (;;) {
        rec = buddy_mic_apply_toggle(rec);
        if (rec && buddy_ble_mic_active()) {
            rec = false;                     /* 主机接管，本地采集让位并收屏 */
            s_mic_rec_start = 0;
        }
        if (!rec && !buddy_ble_mic_active()) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (!buddy_audio_ensure(16000)) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        buddy_adpcm_reset(&enc_state);
        ESP_LOGI(TAG, "麦克风上行开始 (16kHz IMA-ADPCM)");
        while (rec || buddy_ble_mic_active()) {
            rec = buddy_mic_apply_toggle(rec);   /* ← 关键：内层也要能收到"停" */
            if (!rec && !buddy_ble_mic_active()) break;
            if (bsp_audio_read(pcm, sizeof(pcm)) != ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            if (rec) {
                /* 本地采集只看串口统计，不发 BLE；bsp_audio_read 本身是阻塞读
                 * （读满 1024 字节 ≈ 32ms），这里不能再 delay —— RX DMA 缓冲
                 * 只有 3×240 帧 ≈ 45ms，拖慢就会溢出把诊断数据搞乱。 */
                continue;
            }
            samples = sizeof(pcm) / sizeof(pcm[0]);
            buddy_adpcm_encode(pcm, samples, adpcm, &enc_state);
            /* 发不出去就丢这一帧 —— BLE 缓冲有限，宁可丢帧也不能阻塞在这里。 */
            if (buddy_ble_send((const char *)adpcm, sizeof(adpcm)) != ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(5));
            }
        }
        ESP_LOGI(TAG, "麦克风上行结束");
    }
}

static void buddy_report_key(const buddy_control_event_t *control)
{
    static const char *const names[] = {"up", "down", "ok"};
    const char *name;
    const char *kind;
    /* 96 字节：加上 ui 上下文后最长约
     * {"cmd":"key","k":"ok","ev":"click","ui":"transcript"}\n */
    char json[96];
    int written;

    if (control == NULL || control->type != BUDDY_CONTROL_KEY ||
        control->data.key.button > BSP_BTN_OK) {
        return;
    }
    name = names[control->data.key.button];
    if (control->data.key.event == BSP_BTN_CLICK) {
        kind = "click";
    } else if (control->data.key.event == BSP_BTN_LONG) {
        kind = "long";
    } else {
        return;
    }
    if (!atomic_load(&s_ble_initialized) || !buddy_ble_is_connected() ||
        !buddy_ble_is_encrypted()) {
        ESP_LOGW(TAG, "按键未上报 %s/%s: init=%d conn=%d enc=%d", name, kind,
                 (int)atomic_load(&s_ble_initialized),
                 (int)buddy_ble_is_connected(),
                 (int)buddy_ble_is_encrypted());
        return;
    }
    /* 带上当前 UI 上下文：主机据此区分「这个 OK 短按是在操作菜单」还是「要开语音输入」。
     * ⚠ 曾经不带 —— 结果在菜单/设置里按 OK，设备进菜单的同时桥还把它当成语音输入开了录音
     *   （科长 10/03 报的「OK 键和硬件按键冲突」）。 */
    {
        const char *ui = "home";
        if (control->data.key.passkey_visible) {
            ui = "passkey";
        } else if (control->data.key.approval_visible) {
            ui = "approval";
        } else if (control->data.key.reset_open) {
            ui = "reset";
        } else if (control->data.key.menu_open) {
            ui = "menu";
        } else if (control->data.key.page == BUDDY_PAGE_SETTINGS) {
            ui = "settings";
        } else if (control->data.key.page == BUDDY_PAGE_TRANSCRIPT) {
            ui = "transcript";
        }
        written = snprintf(json, sizeof(json),
                           "{\"cmd\":\"key\",\"k\":\"%s\",\"ev\":\"%s\",\"ui\":\"%s\"}\n",
                           name, kind, ui);
    }
    if (written > 0 && (size_t)written < sizeof(json)) {
        esp_err_t send_rc = buddy_ble_send(json, (size_t)written);
        ESP_LOGW(TAG, "按键上报 %s/%s send_rc=%d", name, kind, (int)send_rc);
    }
}

static bool buddy_translate_key(const buddy_control_event_t *control, buddy_event_t *event)
{
    if (control == NULL || event == NULL || control->type != BUDDY_CONTROL_KEY) {
        return false;
    }
    memset(event, 0, sizeof(*event));
    switch (control->data.key.button) {
    case BSP_BTN_UP:
        event->key = BUDDY_KEY_UP;
        break;
    case BSP_BTN_DOWN:
        event->key = BUDDY_KEY_DOWN;
        break;
    case BSP_BTN_OK:
        event->key = BUDDY_KEY_OK;
        break;
    default:
        return false;
    }
    if (control->data.key.event == BSP_BTN_CLICK) {
        event->type = BUDDY_EVENT_KEY_CLICK;
    } else if (control->data.key.event == BSP_BTN_LONG) {
        event->type = BUDDY_EVENT_KEY_LONG;
    } else if (control->data.key.event == BSP_BTN_DOUBLE) {
        event->type = BUDDY_EVENT_KEY_DOUBLE;
    } else {
        return false;
    }

    if (control->data.key.approval_visible && control->data.key.prompt_id[0] != '\0') {
        event->has_observed_prompt_id = true;
        buddy_copy_text(event->observed_prompt_id, sizeof(event->observed_prompt_id),
                        control->data.key.prompt_id);
        event->observed_prompt_id_length = strlen(event->observed_prompt_id);
    } else if (event->type == BUDDY_EVENT_KEY_CLICK && event->key == BUDDY_KEY_OK) {
        event->has_observed_prompt_id = true;
    }
    return true;
}

static bool buddy_control_to_event(const buddy_control_event_t *control,
                                   const buddy_state_t *state, buddy_event_t *event)
{
    memset(event, 0, sizeof(*event));
    if (control->type == BUDDY_CONTROL_KEY) {
        return buddy_key_matches_rendered_view(control) && buddy_key_matches_state(control, state) &&
               buddy_translate_key(control, event);
    }
    switch (control->type) {
    case BUDDY_CONTROL_BLE_CONNECTED:
        event->type = BUDDY_EVENT_BLE_CONNECTED;
        event->ble.connection_generation = control->data.ble.connection_generation;
        break;
    case BUDDY_CONTROL_BLE_DISCONNECTED:
        event->type = BUDDY_EVENT_BLE_DISCONNECTED;
        event->ble.status = control->data.ble.status;
        event->ble.connection_generation = control->data.ble.connection_generation;
        break;
    case BUDDY_CONTROL_BLE_PASSKEY:
        event->type = BUDDY_EVENT_BLE_PASSKEY;
        event->ble.passkey = control->data.ble.passkey;
        event->ble.connection_generation = control->data.ble.connection_generation;
        break;
    case BUDDY_CONTROL_BLE_ENCRYPTION:
        event->type = BUDDY_EVENT_BLE_ENCRYPTION;
        event->ble.status = control->data.ble.status;
        event->ble.secure = control->data.ble.secure;
        event->ble.connection_generation = control->data.ble.connection_generation;
        break;
    case BUDDY_CONTROL_BOND_DELETE_RESULT:
        event->type = BUDDY_EVENT_BOND_DELETE_RESULT;
        event->ble.status = control->data.ble.status;
        event->ble.success = control->data.ble.success;
        break;
    case BUDDY_CONTROL_KEY:
        return false;
    }
    return true;
}

static void buddy_sample_battery(buddy_state_t *state)
{
    int percent;
    int millivolts;

    if (!s_initial_battery_available) {
        state->battery_available = false;
        return;
    }
    percent = bsp_battery_soc();
    millivolts = bsp_battery_mv();
    if (percent < 0 || percent > 100 || millivolts < 0 || millivolts > UINT16_MAX) {
        state->battery_available = false;
        return;
    }
    state->battery_available = true;
    state->battery_percent = (uint8_t)percent;
    state->battery_mv = (uint16_t)millivolts;
}

static void buddy_reset_transient_state(buddy_state_t *state, const char *message)
{
    buddy_settings_snapshot_t settings;
    bool battery_available = state->battery_available;
    uint8_t battery_percent = state->battery_percent;
    uint16_t battery_mv = state->battery_mv;

    if (buddy_settings_load(&settings) != ESP_OK) {
        settings = state->settings;
    }
    if (settings.name[0] == '\0') {
        buddy_default_name(settings.name);
    }
    buddy_state_init(state, &settings);
    state->battery_available = battery_available;
    state->battery_percent = battery_percent;
    state->battery_mv = battery_mv;
    state->ble_connected = buddy_ble_is_connected();
    state->ble_encrypted = buddy_ble_is_encrypted();
    buddy_copy_text(state->message, sizeof(state->message), message);
}

static esp_err_t buddy_ble_ensure_initialized(void)
{
    const buddy_ble_config_t config = {
        .event_cb = on_ble_event,
        .event_context = NULL,
    };
    esp_err_t err;

    if (atomic_load(&s_ble_initialized)) {
        return ESP_OK;
    }
    err = buddy_ble_init(&config);
    if (err == ESP_OK) {
        atomic_store(&s_ble_initialized, true);
    }
    return err;
}

static uint64_t buddy_queue_overflow_total(void)
{
    return (uint64_t)atomic_load_explicit(&s_control_coalesced, memory_order_relaxed) +
           (uint64_t)atomic_load_explicit(&s_button_dropped, memory_order_relaxed) +
           (uint64_t)atomic_load_explicit(&s_rx_normal_coalesced, memory_order_relaxed) +
           (uint64_t)atomic_load_explicit(&s_rx_priority_evicted, memory_order_relaxed) +
           (uint64_t)atomic_load_explicit(&s_rx_dropped, memory_order_relaxed);
}

static esp_err_t buddy_transport_start(void *context)
{
    (void)context;
    return buddy_ble_start();
}

static esp_err_t buddy_transport_stop(void *context)
{
    (void)context;
    return buddy_ble_stop();
}

static esp_err_t buddy_set_ble_enabled(buddy_state_t *state, bool enabled)
{
    const buddy_app_ble_transport_ops_t ops = {
        .context = NULL,
        .start = buddy_transport_start,
        .stop = buddy_transport_stop,
    };
    buddy_app_ble_transport_result_t result = {
        .request_status = ESP_OK,
        .effective_enabled = enabled,
    };

    if (buddy_settings_set_ble_enabled(enabled) != ESP_OK) {
        state->settings.ble_enabled = !enabled;
        buddy_copy_text(state->message, sizeof(state->message), "BLE setting failed");
        return ESP_FAIL;
    }
    if (enabled) {
        result.request_status = buddy_ble_ensure_initialized();
        if (result.request_status == ESP_OK) {
            result = buddy_app_set_ble_transport(&ops, true);
        } else {
            result.effective_enabled = false;
        }
    } else if (atomic_load(&s_ble_initialized)) {
        result = buddy_app_set_ble_transport(&ops, false);
    }
    if (result.request_status != ESP_OK) {
        if (buddy_settings_set_ble_enabled(result.effective_enabled) != ESP_OK ||
            buddy_settings_flush(true) != ESP_OK) {
            buddy_copy_text(state->message, sizeof(state->message),
                            "BLE rollback failed");
        } else {
            buddy_copy_text(state->message, sizeof(state->message),
                            result.recovery_attempted && result.effective_enabled
                                ? "BLE stop failed; restored"
                                : "BLE update failed");
        }
        state->settings.ble_enabled = result.effective_enabled;
        return result.request_status;
    }
    state->settings.ble_enabled = enabled;
    (void)buddy_settings_flush(true);
    return ESP_OK;
}

static esp_err_t buddy_factory_reset(buddy_state_t *state)
{
    buddy_settings_snapshot_t defaults = {0};

    if (buddy_settings_factory_reset() != ESP_OK) {
        buddy_copy_text(state->message, sizeof(state->message), "Factory reset failed");
        return ESP_FAIL;
    }
    defaults.ble_enabled = true;
    buddy_default_name(defaults.name);
    if (buddy_settings_set_name(defaults.name) != ESP_OK ||
        buddy_settings_flush(true) != ESP_OK) {
        buddy_copy_text(state->message, sizeof(state->message),
                        "Factory defaults save failed");
        return ESP_FAIL;
    }
    buddy_state_init(state, &defaults);
    buddy_sample_battery(state);
    buddy_copy_text(state->message, sizeof(state->message), "Factory reset complete");
    if (!atomic_load(&s_ble_initialized)) {
        buddy_set_ble_enabled(state, true);
    } else if (buddy_ble_start() != ESP_OK) {
        buddy_copy_text(state->message, sizeof(state->message), "BLE restart failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static bool buddy_orchestrator_generation_secure(void *context, uint32_t generation)
{
    (void)context;
    return buddy_ble_is_generation_secure(generation);
}

static esp_err_t buddy_orchestrator_send(void *context, const char *data, size_t length,
                                         uint32_t generation)
{
    (void)context;
    return buddy_ble_send_for_generation(data, length, generation);
}

static esp_err_t buddy_orchestrator_commit_name(void *context, const char *name)
{
    (void)context;
    return buddy_settings_set_name_committed(name);
}

static esp_err_t buddy_orchestrator_commit_owner(void *context, const char *owner)
{
    (void)context;
    return buddy_settings_set_owner_committed(owner);
}

static esp_err_t buddy_orchestrator_status(void *context, const buddy_state_t *state,
                                           buddy_status_report_t *report)
{
    buddy_settings_snapshot_t settings;
    buddy_app_status_runtime_t runtime = {
        .encrypted = atomic_load(&s_ble_initialized) && buddy_ble_is_encrypted(),
        .battery_available = state->battery_available,
        .battery_percent = state->battery_percent,
        .battery_mv = state->battery_mv,
        .uptime_ms = buddy_now_ms(),
        .free_heap = esp_get_free_heap_size(),
        .queue_overflow_count = buddy_queue_overflow_total(),
    };

    (void)context;
    if (buddy_settings_load(&settings) != ESP_OK ||
        !buddy_app_build_status(report, &settings, &runtime)) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void buddy_orchestrator_record_permission(void *context,
                                                 buddy_permission_decision_t decision)
{
    (void)context;
    buddy_settings_record_permission(decision);
}

static esp_err_t buddy_orchestrator_unpair(void *context)
{
    buddy_state_t *state = context;

    if (buddy_ble_ensure_initialized() != ESP_OK || buddy_ble_delete_bonds() != ESP_OK) {
        buddy_copy_text(state->message, sizeof(state->message), "Unpair failed");
        return ESP_FAIL;
    }
    buddy_reset_transient_state(state, "Unpairing");
    return ESP_OK;
}

static esp_err_t buddy_orchestrator_factory_reset(void *context)
{
    return buddy_factory_reset(context);
}

static esp_err_t buddy_orchestrator_set_ble(void *context, bool enabled)
{
    return buddy_set_ble_enabled(context, enabled);
}

static esp_err_t buddy_orchestrator_persist_level(void *context, uint64_t level)
{
    (void)context;
    return buddy_settings_set_highest_celebrated_level(level);
}

static buddy_orchestrator_ops_t buddy_orchestrator_ops(buddy_state_t *state)
{
    const buddy_orchestrator_ops_t ops = {
        .context = state,
        .generation_secure = buddy_orchestrator_generation_secure,
        .send = buddy_orchestrator_send,
        .commit_name = buddy_orchestrator_commit_name,
        .commit_owner = buddy_orchestrator_commit_owner,
        .status_report = buddy_orchestrator_status,
        .record_permission = buddy_orchestrator_record_permission,
        .unpair = buddy_orchestrator_unpair,
        .factory_reset = buddy_orchestrator_factory_reset,
        .set_ble_enabled = buddy_orchestrator_set_ble,
        .persist_level = buddy_orchestrator_persist_level,
    };
    return ops;
}

/* 10/03 晚：菜单里「要活过重启」的几项（亮度/背光/钟向/音量/播报/提示音）落 NVS。
 * 幂等 —— 与上次写过的值一致就直接返回，免得每次按键都擦写 flash。 */
static void buddy_persist_ui_state(const buddy_state_t *state)
{
    static buddy_settings_snapshot_t saved;
    buddy_settings_snapshot_t snap;

    if (state == NULL) {
        return;
    }
    if (state->brightness_level == saved.brightness_level &&
        state->led_effect == saved.led_effect &&
        state->rotation == saved.rotation &&
        state->volume == saved.volume &&
        state->speak_enabled == saved.speak_enabled &&
        state->beep_enabled == saved.beep_enabled) {
        return;
    }
    snap = state->settings;
    snap.brightness_level = state->brightness_level;
    snap.led_effect = state->led_effect;
    snap.rotation = state->rotation;
    snap.volume = state->volume;
    snap.speak_enabled = state->speak_enabled;
    snap.beep_enabled = state->beep_enabled;
    if (buddy_settings_set_ui_state(&snap) == ESP_OK) {
        saved = snap;
    }
}

/* 把菜单开关送给桥（10/03 晚）。
 * ⚠ 必须带**当前连接代号** —— buddy_ble_send_for_generation 会校验，而按键产生的
 *   action 里那个字段是 0，直接发会被拦掉（连不上就静默失败）。 */
static void buddy_send_setting(const char *key, bool value)
{
    char json[BUDDY_PROTOCOL_TX_MAX];
    int length = buddy_protocol_setting_json(json, sizeof(json), key, value);

    if (length > 0) {
        (void)buddy_ble_send_for_generation(json, (size_t)length,
                                            buddy_ble_current_generation());
    }
}

static bool buddy_execute_action(buddy_state_t *state, const buddy_action_t *action,
                                 buddy_event_t *result_event)
{
    buddy_orchestrator_ops_t ops = buddy_orchestrator_ops(state);

    buddy_persist_ui_state(state);

    if (action->type == BUDDY_ACTION_VOLUME) {
        /* 音量纯设备侧：直接调喇叭，不经过桥 */
        bsp_audio_set_volume(state->volume);
        memset(result_event, 0, sizeof(*result_event));
        return false;
    }
    if (action->type == BUDDY_ACTION_SETTING_SYNC) {
        buddy_send_setting(action->setting_key, action->setting_value);
        memset(result_event, 0, sizeof(*result_event));
        return false;
    }
    if (action->type == BUDDY_ACTION_RESET_SETTINGS) {
        /* 「重置设置」（10/03 晚）：菜单那六项回默认值，屏幕与喇叭立刻生效并落 NVS。
         * 播报/提示音是桥在执行，所以还要把这两个开关送回桥，否则它下次重连会把值盖回去。 */
        state->brightness_level = 2U;
        state->led_effect = 1U;
        state->rotation = 0U;
        state->volume = 80U;
        state->speak_enabled = true;
        state->beep_enabled = true;
        bsp_display_backlight((uint8_t)(20U + state->brightness_level * 20U));
        bsp_audio_set_volume(state->volume);
        buddy_persist_ui_state(state);
        buddy_send_setting("speak", state->speak_enabled);
        buddy_send_setting("beep", state->beep_enabled);
        memset(result_event, 0, sizeof(*result_event));
        return false;
    }
    if (action->type == BUDDY_ACTION_DISPLAY_BACKLIGHT) {
        s_screen_off = false;          /* 唤醒/调亮度：解除息屏冻结 */
        ESP_LOGI(TAG, "action DISPLAY_BACKLIGHT：解除冻结 -> %u%%",
                 (unsigned)action->brightness_percent);
        bsp_display_backlight(action->brightness_percent);
        memset(result_event, 0, sizeof(*result_event));
        return false;
    }
    if (action->type == BUDDY_ACTION_SCREEN_OFF) {
        s_screen_off = true;           /* 见 s_screen_off 注释：不设它会立刻被呼吸灯覆盖 */
        ESP_LOGI(TAG, "action SCREEN_OFF：冻结背光到 0%%（s_screen_off=1）");
        bsp_display_backlight(0);
        memset(result_event, 0, sizeof(*result_event));
        return false;
    }

    (void)buddy_orchestrator_execute_action(state, &ops, action, result_event);
    if (result_event->type == BUDDY_EVENT_PERMISSION_SEND_RESULT &&
        !result_event->permission_result.success) {
        ESP_LOGW(TAG, "permission response failed");
    }
    return result_event->type == BUDDY_EVENT_PERMISSION_SEND_RESULT;
}

static bool buddy_rendered_view_same(const buddy_rendered_view_t *left,
                                     const buddy_rendered_view_t *right)
{
    return left->page == right->page && left->confirmation == right->confirmation &&
           left->settings_selection == right->settings_selection &&
           left->menu_selection == right->menu_selection &&
           left->reset_selection == right->reset_selection &&
           left->menu_open == right->menu_open &&
           left->reset_open == right->reset_open &&
           left->approval_visible == right->approval_visible &&
           left->passkey_visible == right->passkey_visible &&
           left->ble_enabled == right->ble_enabled &&
           left->sensitive_connection_generation ==
               right->sensitive_connection_generation &&
           strcmp(left->prompt_id, right->prompt_id) == 0 &&
           strcmp(left->prompt_tool, right->prompt_tool) == 0 &&
           strcmp(left->prompt_hint, right->prompt_hint) == 0;
}

static void buddy_publish_rendered_view(const buddy_ui_snapshot_t *snapshot)
{
    buddy_rendered_view_t next = {
        .page = snapshot->page,
        .confirmation = snapshot->confirmation,
        .settings_selection = snapshot->settings_selection,
        .menu_selection = snapshot->menu_selection,
        .reset_selection = snapshot->reset_selection,
        .menu_open = snapshot->menu_open,
        .reset_open = snapshot->reset_open,
        .approval_visible = !snapshot->confirmation_pending && !snapshot->passkey_visible &&
                            !snapshot->approval_locked && snapshot->prompt_id[0] != '\0',
        .passkey_visible = snapshot->passkey_visible,
        .ble_enabled = snapshot->ble_enabled,
        .sensitive_connection_generation = snapshot->confirmation_pending
                                               ? snapshot->confirmation_connection_generation
                                               : snapshot->prompt_connection_generation,
    };

    if (next.approval_visible) {
        buddy_copy_text(next.prompt_id, sizeof(next.prompt_id), snapshot->prompt_id);
        buddy_copy_text(next.prompt_tool, sizeof(next.prompt_tool), snapshot->prompt_tool);
        buddy_copy_text(next.prompt_hint, sizeof(next.prompt_hint), snapshot->prompt_hint);
    }
    taskENTER_CRITICAL(&s_view_lock);
    if (!buddy_rendered_view_same(&next, &s_rendered_view)) {
        next.generation = s_rendered_view.generation + 1U;
        if (next.generation == 0U) {
            next.generation = 1U;
        }
    } else {
        next.generation = s_rendered_view.generation != 0U ? s_rendered_view.generation : 1U;
    }
    s_rendered_view = next;
    taskEXIT_CRITICAL(&s_view_lock);
}

static void buddy_render(buddy_state_t *state, const buddy_action_t *action, uint64_t now_ms)
{
    static buddy_ui_snapshot_t snapshot;

    buddy_state_snapshot(state, &snapshot);
    buddy_apply_display_settings(&snapshot, now_ms);
    buddy_notify_new_body(&snapshot);
    if (!bsp_lvgl_lock(1000)) {
        return;
    }
    buddy_ui_render(&snapshot);
    /* 转圈指示：主机 mic_start 期间（buddy_ble_mic_active）或本地采集期间都亮。 */
    buddy_ui_set_recording(buddy_ble_mic_active() ? 0 : buddy_mic_rec_seconds());
    if (action->type == BUDDY_ACTION_UI_SCROLL) {
        buddy_ui_scroll(action->scroll_delta);
    }
    buddy_ui_tick(now_ms);
    bsp_lvgl_unlock();
    buddy_publish_rendered_view(&snapshot);
}

/* {"cmd":"volume","pct":N} —— 让主机不刷固件就能调喇叭音量。
 * 在这里嗅探而不是塞进状态机：音量是"执行"类动作，与 UI 状态无关。 */
static void buddy_try_volume_command(const char *line, size_t length)
{
    const char *cmd_key = "\"cmd\":\"volume\"";
    const char *pct_key = "\"pct\":";
    size_t i;

    if (line == NULL) {
        return;
    }
    for (i = 0U; i + 14U <= length; ++i) {
        if (memcmp(line + i, cmd_key, 14U) == 0) {
            break;
        }
    }
    if (i + 14U > length) {
        return;
    }
    for (i = 0U; i + 6U <= length; ++i) {
        if (memcmp(line + i, pct_key, 6U) == 0) {
            long pct = strtol(line + i + 6U, NULL, 10);

            if (pct < 0L) {
                pct = 0L;
            } else if (pct > 100L) {
                pct = 100L;
            }
            bsp_audio_set_volume((uint8_t)pct);
            return;
        }
    }
}

/* {"cmd":"settings","speak":true,"beep":false} —— 桥把开关状态推下来（10/03 晚）。
 * 桥是「播报/提示音」的执行者，所以以它为准：设备菜单只是遥控器，避免两边显示打架。 */
/* 10/03 晚：桥在 BLE 连接建立后会把最近几条**摘要**逐条补推下来，
 * 只为把设备的翻页历史填回去（设备重启会清空）—— 不切页面、不动当前正文。
 * 命令：{"cmd":"history_add","body":"…"}\n
 * 桥按「从旧到新」的顺序发，最后一条自然成为 history[0]（最新）。 */
static void buddy_try_history_command(buddy_state_t *state, const char *line, size_t length)
{
    static const char *const prefix = "{\"cmd\":\"history_add\",";
    cJSON *root;
    const cJSON *cmd;
    const cJSON *body;

    if (state == NULL || line == NULL || length < sizeof("{\"cmd\":\"history_add\",") - 1U) {
        return;
    }
    /* 粗筛（绝大多数 RX 都不是这条命令，别白白 parse） */
    if (memcmp(line, prefix, sizeof("{\"cmd\":\"history_add\",") - 1U) != 0) {
        return;
    }
    root = cJSON_ParseWithLength(line, length);
    if (root == NULL) {
        return;
    }
    cmd = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    body = cJSON_GetObjectItemCaseSensitive(root, "body");
    if (cJSON_IsString(cmd) && strcmp(cmd->valuestring, "history_add") == 0 &&
        cJSON_IsString(body) && body->valuestring != NULL) {
        buddy_state_push_history(state, body->valuestring);
    }
    cJSON_Delete(root);
}

static void buddy_try_settings_command(buddy_state_t *state, const char *line, size_t length)
{
    static const char *const cmd_key = "\"cmd\":\"settings\"";
    static const char *const speak_key = "\"speak\":";
    static const char *const beep_key = "\"beep\":";
    bool changed = false;
    size_t i;

    if (state == NULL || line == NULL || length < sizeof("\"cmd\":\"settings\"") - 1U) {
        return;
    }
    for (i = 0U; i + 16U <= length; ++i) {
        if (memcmp(line + i, cmd_key, 16U) == 0) {
            break;
        }
    }
    if (i + 16U > length) {
        return;
    }
    for (i = 0U; i + 8U <= length; ++i) {
        if (memcmp(line + i, speak_key, 8U) == 0) {
            bool value = strncmp(line + i + 8U, "true", 4U) == 0;

            if (state->speak_enabled != value) {
                state->speak_enabled = value;
                changed = true;
            }
            break;
        }
    }
    for (i = 0U; i + 7U <= length; ++i) {
        if (memcmp(line + i, beep_key, 7U) == 0) {
            bool value = strncmp(line + i + 7U, "true", 4U) == 0;

            if (state->beep_enabled != value) {
                state->beep_enabled = value;
                changed = true;
            }
            break;
        }
    }
    if (changed) {
        buddy_persist_ui_state(state);   /* 幂等：值没变不写 flash */
    }
}

static bool buddy_handle_rx(buddy_state_t *state, buddy_rx_slot_t *slot,
                            buddy_event_t *event, uint64_t now_ms,
                            buddy_action_t *action)
{
    buddy_orchestrator_ops_t ops = buddy_orchestrator_ops(state);

    buddy_try_volume_command(slot->data, slot->length);
    buddy_try_settings_command(state, slot->data, slot->length);
    buddy_try_history_command(state, slot->data, slot->length);
    (void)event;
    return buddy_orchestrator_process_rx(state, &ops, slot->data, slot->length,
                                         slot->connection_generation, now_ms, action);
}

static QueueHandle_t buddy_next_ready_queue(void)
{
    static QueueHandle_t *const ordered[] = {
        &s_link_queue,
        &s_passkey_queue,
        &s_security_queue,
        &s_bond_queue,
        &s_rx_priority_queue,
        &s_button_queue,
        &s_rx_normal_queue,
    };
    size_t index;

    for (index = 0; index < sizeof(ordered) / sizeof(ordered[0]); ++index) {
        if (*ordered[index] != NULL && uxQueueMessagesWaiting(*ordered[index]) != 0U) {
            return *ordered[index];
        }
    }
    return NULL;
}

static QueueHandle_t buddy_wait_for_queue(void)
{
    QueueHandle_t ready = buddy_next_ready_queue();

    if (ready == NULL) {
        /* 本地采集期间把空转周期缩短，好让状态栏的"转圈圈"转起来（见 buddy_ui_tick）。 */
        uint32_t idle_ms = (s_mic_rec_start != 0) ? 60U : BUDDY_APP_TICK_MS;
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(idle_ms));
        ready = buddy_next_ready_queue();
    }
    return ready;
}

static void buddy_app_task(void *context)
{
    static buddy_state_t state;
    static buddy_action_t action;
    static buddy_event_t event;
    uint64_t last_battery_ms = 0;
    uint64_t last_settings_ms = 0;

    (void)context;
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    buddy_state_init(&state, &s_initial_settings);
    buddy_sample_battery(&state);

    for (;;) {
        QueueHandle_t ready = buddy_wait_for_queue();
        uint64_t now_ms = buddy_now_ms();
        bool reduced = false;

        memset(&action, 0, sizeof(action));

        if (ready == s_link_queue || ready == s_passkey_queue ||
            ready == s_security_queue || ready == s_bond_queue ||
            ready == s_button_queue) {
            buddy_control_event_t control;

            if (xQueueReceive(ready, &control, 0) == pdTRUE) {
                if (ready == s_button_queue) {
                    buddy_report_key(&control);
                }
                if (buddy_control_to_event(&control, &state, &event)) {
                    buddy_state_reduce(&state, &event, now_ms, &action);
                    reduced = true;
                }
            }
        } else if (ready == s_rx_priority_queue || ready == s_rx_normal_queue) {
            buddy_rx_slot_t *slot = NULL;

            if (xQueueReceive(ready, &slot, 0) == pdTRUE && slot != NULL) {
                reduced = buddy_handle_rx(&state, slot, &event, now_ms, &action);
                buddy_rx_slot_release(slot);
            }
        }
        if (!reduced) {
            const buddy_event_t tick = {.type = BUDDY_EVENT_TICK};

            buddy_state_reduce(&state, &tick, now_ms, &action);
        }

        if (buddy_execute_action(&state, &action, &event)) {
            buddy_state_reduce(&state, &event, now_ms, &action);
        }
        if (now_ms - last_battery_ms >= BUDDY_BATTERY_SAMPLE_MS) {
            buddy_sample_battery(&state);
            last_battery_ms = now_ms;
        }
        if (now_ms - last_settings_ms >= BUDDY_SETTINGS_SERVICE_MS) {
            if (buddy_settings_flush(false) != ESP_OK) {
                ESP_LOGW(TAG, "settings flush failed");
            }
            last_settings_ms = now_ms;
        }
        state.ble_connected = atomic_load(&s_ble_initialized) && buddy_ble_is_connected();
        state.ble_encrypted = atomic_load(&s_ble_initialized) && buddy_ble_is_encrypted();
        buddy_render(&state, &action, now_ms);
    }
}

static esp_err_t buddy_nvs_init(void)
{
    esp_err_t err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        err = nvs_flash_erase();
        if (err == ESP_OK) {
            err = nvs_flash_init();
        }
    }
    return err;
}

void app_main(void)
{
    esp_err_t err;

    ESP_LOGI(TAG, "Hermes Buddy starting");
    if (buddy_nvs_init() != ESP_OK) {
        ESP_LOGE(TAG, "NVS initialization failed");
        return;
    }
    err = bsp_i2c_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "I2C initialization failed: %s", esp_err_to_name(err));
    }
    if (bsp_display_init() != ESP_OK || bsp_lvgl_init() == NULL) {
        ESP_LOGE(TAG,
                 "Display/LVGL initialization failed (MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    bsp_display_backlight(100);
    /* FAP_SCREENSHOT_V1 串口截屏监听（只读观察，抓帧不占帧缓冲）。
     * ⚠ 诊断开关（10/04）：这个任务的读任务与 console 日志抢同一条 CDC 流，
     *   会把启动日志整个吞掉（主机读串口永远是 0 字节）。
     *   抓启动日志时置 0；平时置 1 —— 置 0 就等于放弃截屏能力。 */
#if 1  /* ⚠ 诊断期临时改为 0：抓完启动日志必须改回 1 并重刷 */
    if (fap_screenshot_start() != ESP_OK) {
        ESP_LOGW(TAG, "screenshot listener unavailable");
    }
#endif
    s_initial_battery_available = bsp_battery_init() == ESP_OK;

    if (buddy_settings_init() != ESP_OK || buddy_settings_load(&s_initial_settings) != ESP_OK) {
        ESP_LOGE(TAG, "settings initialization failed");
        return;
    }
    /* 10/03 晚：按 NVS 里存的亮度重新点一次屏（前面那次是在设置加载之前，固定 100%）。 */
    bsp_display_backlight((uint8_t)(20U +
        (uint8_t)(s_initial_settings.brightness_level % 5U) * 20U));
    if (s_initial_settings.name[0] == '\0') {
        buddy_default_name(s_initial_settings.name);
        if (buddy_settings_set_name(s_initial_settings.name) != ESP_OK ||
            buddy_settings_flush(true) != ESP_OK) {
            ESP_LOGE(TAG, "default name persistence failed");
            return;
        }
    }

    s_link_queue = xQueueCreate(BUDDY_CRITICAL_QUEUE_DEPTH, sizeof(buddy_control_event_t));
    s_passkey_queue = xQueueCreate(BUDDY_CRITICAL_QUEUE_DEPTH,
                                   sizeof(buddy_control_event_t));
    s_security_queue = xQueueCreate(BUDDY_CRITICAL_QUEUE_DEPTH,
                                    sizeof(buddy_control_event_t));
    s_bond_queue = xQueueCreate(BUDDY_CRITICAL_QUEUE_DEPTH, sizeof(buddy_control_event_t));
    s_button_queue = xQueueCreate(BUDDY_BUTTON_QUEUE_DEPTH, sizeof(buddy_control_event_t));
    s_rx_normal_queue = xQueueCreate(BUDDY_RX_NORMAL_QUEUE_DEPTH,
                                     sizeof(buddy_rx_slot_t *));
    s_rx_priority_queue = xQueueCreate(BUDDY_RX_PRIORITY_QUEUE_DEPTH,
                                       sizeof(buddy_rx_slot_t *));
    if (s_link_queue == NULL || s_passkey_queue == NULL || s_security_queue == NULL ||
        s_bond_queue == NULL || s_button_queue == NULL || s_rx_normal_queue == NULL ||
        s_rx_priority_queue == NULL ||
        xTaskCreate(buddy_app_task, "buddy_app", BUDDY_APP_STACK_SIZE, NULL,
                    BUDDY_APP_PRIORITY, &s_app_task_handle) != pdPASS) {
        ESP_LOGE(TAG, "application queue/task initialization failed");
        return;
    }
    if (xTaskCreate(buddy_audio_task, "buddy_audio", BUDDY_AUDIO_STACK_SIZE, NULL,
                    BUDDY_AUDIO_PRIORITY, NULL) != pdPASS) {
        ESP_LOGW(TAG, "audio task creation failed");
    }
    if (xTaskCreate(buddy_mic_task, "buddy_mic", BUDDY_MIC_STACK_SIZE, NULL,
                    BUDDY_AUDIO_PRIORITY, NULL) != pdPASS) {
        ESP_LOGW(TAG, "mic task creation failed");
    }
    if (bsp_button_init(on_key, NULL) != ESP_OK) {
        ESP_LOGW(TAG, "button initialization failed; approvals remain fail-closed");
    }
    if (s_initial_settings.ble_enabled) {
        err = buddy_ble_ensure_initialized();
        if (err == ESP_OK) {
            err = buddy_ble_start();
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "BLE initialization failed: %s", esp_err_to_name(err));
        }
    }
    atomic_store_explicit(&s_app_ready, true, memory_order_release);
    xTaskNotifyGive(s_app_task_handle);

    ESP_LOGI(TAG, "HEAP free=%u largest8=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}
