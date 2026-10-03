// components/bsp/src/bsp_audio.c
// 以小智固件（folo-ai-passport-xiaozhi / Es8311AudioCodec）为参照重写。
//
// 【本次重构的唯一结构差异】
//   旧实现：每次 bsp_audio_set_format() 都 audio_delete_codec()（esp_codec_dev_delete +
//           audio_codec_delete_codec_if）+ audio_create_codec() 重建 —— 播放与录音每次
//           切换用途都把 codec 拆掉重装，codec 生命周期被反复走一遍
//           es8311_close(→suspend) → es8311_open → es8311_enable(→start)，
//           每次重建还要先做一次软件复位（REG00=0x1F + 5ms）。
//   新实现：codec 接口（s_codec/s_ctrl/s_data/s_gpio）与 esp_codec_dev 实例（s_dev）
//           只建一次、常驻；set_format 只在"从未打开"或"采样格式真的变了"时才
//           close + open。这与小智 EnableInput/EnableOutput 的幂等语义一致
//           （小智：`if (enable == input_enabled_) return;`）。
//
//   对齐小智且保持不动的部分：裸 ctrl_if / data_if（不用包装层）、
//   es8311_codec_new 之前先软件复位 REG00=0x1F + 5ms、codec_mode=BOTH、
//   use_mclk=true、pa_voltage=5.0 / codec_dac_voltage=3.3、
//   fs={16bit, ch=1, channel_mask=0, rate, mclk_multiple=0}、set_in_gain(30dB)、
//   dma_desc_num=3（C3 内存约束，见 i2s_full_duplex_init 注释）。
//
//   sleep / deep-sleep 路径保留原实现（自造寄存器序列比驱动 suspend 更彻底，
//   回读校验 + 失败重试一次）。

#include "bsp_audio.h"
#include "bsp_es8311_sleep_check.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es8311_codec.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "bsp_audio";

#define AUDIO_DEFAULT_HZ      16000
#define AUDIO_DEFAULT_BITS    16
#define AUDIO_DEFAULT_CH      1
#define AUDIO_INPUT_GAIN_DB   30.0f
#define ES8311_SLEEP_ATTEMPTS 2
#define ES8311_SLEEP_RETRY_MS 5

static esp_codec_dev_handle_t s_dev;
static i2s_chan_handle_t      s_tx, s_rx;
static const audio_codec_ctrl_if_t *s_ctrl;
static const audio_codec_data_if_t *s_data;
static const audio_codec_if_t      *s_codec;
static const audio_codec_gpio_if_t *s_gpio;
// 对齐小智的 data_if_mutex_：串行化 open/close/格式切换与音量设置。
static SemaphoreHandle_t s_lock;
static bool      s_opened;
static bool      s_sleeping;
static bool      s_initialized;
static esp_err_t s_sleep_result;
static bool      s_codec_release_failed;
static uint8_t   s_volume = 100;
static uint32_t  s_hz;
static uint8_t   s_bits, s_ch;

static void audio_lock(void)
{
    if (s_lock) (void)xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void audio_unlock(void)
{
    if (s_lock) (void)xSemaphoreGive(s_lock);
}

typedef struct {
    uint8_t reg;
    uint8_t value;
} es8311_reg_value_t;

// 寄存器序列不依赖 esp_codec_dev 的 opened 标志。REG45=0x01 额外关闭
// BCLK/LRCK 内部上拉，比驱动自带 suspend（REG45=0x00）更彻底。
static const es8311_reg_value_t s_es8311_sleep_sequence[] = {
    {0x32, 0x00}, {0x17, 0x00}, {0x0E, 0xFF}, {0x12, 0x02},
    {0x14, 0x00}, {0x0D, 0xFA}, {0x15, 0x00}, {0x02, 0x10},
    {0x00, 0x00}, {0x00, 0x1F}, {0x01, 0x30}, {0x01, 0x00},
    {0x45, 0x01}, {0x0D, 0xFC}, {0x02, 0x00},
};

// 关键寄存器快照（诊断用）：串口里一眼看出 ES8311 停在哪个状态。
// 正常"已开麦"应为 REG00=80 REG09=0C REG0A=0C REG12=00 REG14=1A REG17=BF REG44=58。
static void es8311_log_regs(const char *when)
{
    if (!s_ctrl || !s_ctrl->read_reg) return;
    static const uint8_t regs[] = {
        0x00, 0x01, 0x02, 0x06, 0x09, 0x0A, 0x0D, 0x0E, 0x12,
        0x14, 0x15, 0x16, 0x17, 0x1B, 0x1C, 0x31, 0x32, 0x37, 0x44,
    };
    char line[192];
    int n = snprintf(line, sizeof(line), "ES8311 regs (%s):", when);
    for (size_t i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
        uint8_t v = 0;
        if (s_ctrl->read_reg(s_ctrl, regs[i], 1, &v, 1) != ESP_CODEC_DEV_OK) continue;
        if (n >= (int)sizeof(line) - 10) break;
        n += snprintf(line + n, sizeof(line) - n, " %02X=%02X", regs[i], v);
    }
    ESP_LOGI(TAG, "%s", line);
}

static esp_err_t audio_disable_i2s_channels(void)
{
    esp_err_t first_error = ESP_OK;
    const struct {
        i2s_chan_handle_t channel;
        const char *name;
    } channels[] = {
        {s_tx, "TX"},
        {s_rx, "RX"},
    };

    for (size_t i = 0; i < sizeof(channels) / sizeof(channels[0]); i++) {
        if (!channels[i].channel) continue;
        esp_err_t e = i2s_channel_disable(channels[i].channel);
        if (e == ESP_ERR_INVALID_STATE) e = ESP_OK; // READY 即已停止。
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "I2S %s 停止失败: %s", channels[i].name, esp_err_to_name(e));
            if (first_error == ESP_OK) first_error = e;
        }
    }
    return first_error;
}

// esp_codec_dev_open() 内部重配前会先 i2s_channel_disable，而 disable 要求通道处于
// RUNNING；上一次 close（或刚 init）后通道是 READY，会打 "channel has not been enabled
// yet" 并让重配路径落到错误分支。这里先 enable 一次把通道拉回 RUNNING（已 RUNNING 时
// 返回 ESP_ERR_INVALID_STATE，忽略即可）。
static esp_err_t audio_prepare_i2s_reopen(void)
{
    esp_err_t e = s_tx ? i2s_channel_enable(s_tx) : ESP_ERR_INVALID_STATE;
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "I2S TX 恢复失败: %s", esp_err_to_name(e));
        return e;
    }
    esp_err_t r = s_rx ? i2s_channel_enable(s_rx) : ESP_ERR_INVALID_STATE;
    if (r != ESP_OK && r != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "I2S RX 恢复失败: %s", esp_err_to_name(r));
        if (s_tx) (void)i2s_channel_disable(s_tx);
        return r;
    }
    return ESP_OK;
}

static esp_err_t es8311_force_sleep_once(unsigned attempt)
{
    bool valid = true;

    for (size_t i = 0; i < sizeof(s_es8311_sleep_sequence) /
                           sizeof(s_es8311_sleep_sequence[0]); i++) {
        const es8311_reg_value_t *item = &s_es8311_sleep_sequence[i];
        uint8_t value = item->value;
        int write_result = s_ctrl->write_reg(s_ctrl, item->reg, 1, &value, 1);
        if (write_result == ESP_CODEC_DEV_OK) continue;

        uint8_t actual = 0;
        int read_result = s_ctrl->read_reg(s_ctrl, item->reg, 1, &actual, 1);
        if (read_result == ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "ES8311 休眠写入失败 attempt=%u REG%02X "
                          "expected=0x%02X actual=0x%02X error=%d",
                     attempt, item->reg, item->value, actual, write_result);
        } else {
            ESP_LOGE(TAG, "ES8311 休眠写入失败 attempt=%u REG%02X "
                          "expected=0x%02X actual=unavailable error=%d read_error=%d",
                     attempt, item->reg, item->value, write_result, read_result);
        }
        valid = false;
    }

    for (size_t i = 0; i < bsp_es8311_sleep_check_count; i++) {
        const bsp_es8311_reg_check_t *item = &bsp_es8311_sleep_checks[i];
        uint8_t actual = 0;
        int read_result = s_ctrl->read_reg(s_ctrl, item->reg, 1, &actual, 1);
        if (read_result == ESP_CODEC_DEV_OK &&
            bsp_es8311_sleep_check_matches(item, actual)) continue;

        ESP_LOGE(TAG, "ES8311 休眠校验失败 attempt=%u REG%02X "
                      "expected=0x%02X mask=0x%02X actual=0x%02X error=%d",
                 attempt, item->reg, item->value, item->mask, actual, read_result);
        valid = false;
    }
    return valid ? ESP_OK : ESP_FAIL;
}

static esp_err_t es8311_force_sleep(void)
{
    if (!s_ctrl || !s_ctrl->read_reg || !s_ctrl->write_reg) {
        return ESP_ERR_INVALID_STATE;
    }

    for (unsigned attempt = 1; attempt <= ES8311_SLEEP_ATTEMPTS; attempt++) {
        esp_err_t e = es8311_force_sleep_once(attempt);
        if (e == ESP_OK) {
            ESP_LOGI(TAG, "ES8311 已进入低功耗状态并通过寄存器校验");
            return ESP_OK;
        }
        if (attempt < ES8311_SLEEP_ATTEMPTS) {
            vTaskDelay(pdMS_TO_TICKS(ES8311_SLEEP_RETRY_MS));
        }
    }

    ESP_LOGE(TAG, "ES8311 强制休眠失败");
    return ESP_FAIL;
}

static esp_err_t i2s_full_duplex_init(void)
{
    i2s_chan_config_t chan = {
        .id = BSP_I2S_PORT,
        .role = I2S_ROLE_MASTER,
        /* ⚠ 这个值是「上行内存」与「下行缓冲」的拔河，两个方向需求相反：
         *   · 上行（BLE 录音）在设备配对+加密后内存最紧 —— 3×240 时 RX 那块连续
         *     内存分不出来（i2s rx 初始化失败: ESP_ERR_NO_MEM，以 50ms 周期重试）。
         *   · 下行（播报播放）要尽量大 —— DMA 太小会让 bsp_audio_write 频繁阻塞，
         *     环形缓冲被 BLE 灌溢出，听感就是「播一句就断」。
         *   3×160 修好了上行，但 @16kHz 只有 30ms 缓冲，下行不够（实测设备只送出
         *   应有字节数的 ~78%）。这里回到 3×240（@16kHz 45ms）验下行；失败时下面的
         *   日志会打出当时的堆余量，据此再定两边兼顾的值。 */
        .dma_desc_num = 3,
        .dma_frame_num = 240,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    esp_err_t e = i2s_new_channel(&chan, &s_tx, &s_rx);
    if (e != ESP_OK) { ESP_LOGE(TAG, "i2s_new_channel 失败: %s", esp_err_to_name(e)); return e; }

    // 这里的采样率只用于建通道；实际速率由 esp_codec_dev_open() 按需重配。
    i2s_std_config_t std = {
        .clk_cfg = {
            .sample_rate_hz = AUDIO_DEFAULT_HZ,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .ext_clk_freq_hz = 0,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = I2S_STD_SLOT_BOTH,
            .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = true,
            .big_endian = false,
            .bit_order_lsb = false,
        },
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK, .bclk = BSP_I2S_BCLK, .ws = BSP_I2S_WS,
            .dout = BSP_I2S_DOUT, .din = BSP_I2S_DIN,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    if ((e = i2s_channel_init_std_mode(s_tx, &std)) != ESP_OK) {
        ESP_LOGE(TAG, "i2s tx 初始化失败: %s", esp_err_to_name(e)); return e;
    }
    if ((e = i2s_channel_init_std_mode(s_rx, &std)) != ESP_OK) {
        ESP_LOGE(TAG, "i2s rx 初始化失败: %s (内部最大连续块 %u B / 可用 %u B)",
                 esp_err_to_name(e),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        return e;
    }
    // 对齐小智 CreateDuplexChannels()：init 之后立即 enable 两条通道。
    e = i2s_channel_enable(s_tx);
    if (e == ESP_OK) e = i2s_channel_enable(s_rx);
    if (e != ESP_OK) ESP_LOGE(TAG, "i2s channel enable 失败: %s", esp_err_to_name(e));
    return e;
}

// codec 接口与 esp_codec_dev 实例都只在这里创建一次，之后常驻（小智语义）。
static esp_err_t audio_create_codec(void)
{
    if (s_codec_release_failed) return ESP_ERR_INVALID_STATE;

    /* ⚠ 关键：ES8311 上电后必须先把数字块按在复位态数毫秒，让状态机重新启动。
     *   依据小智 Es8311AudioCodec::ResetCodec() 与 ES8311 初始化指南
     *   （"Hold the ES8311 digital blocks in reset for several milliseconds...
     *    Normal codec initialization releases the reset and starts the state machine"）。
     *   缺这步时寄存器读回看似一切正常(REG00=0x80)，但 ADC 状态机没起来 ——
     *   录音读回的是恒定直流而非语音，正是本项目卡住的现象。 */
    {
        uint8_t reset_value = 0x1F;
        (void)s_ctrl->write_reg(s_ctrl, 0x00, 1, &reset_value, 1);
        vTaskDelay(pdMS_TO_TICKS(5));
        ESP_LOGI(TAG, "ES8311 软件复位完成 (REG00=0x1F, hold 5ms)");
    }

    s_codec = es8311_codec_new(&(es8311_codec_cfg_t){
        .ctrl_if = s_ctrl,   // 对齐小智：裸 ctrl_if，不用包装层
        .gpio_if = s_gpio,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = BSP_I2S_PA_CTRL,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .hw_gain = { .pa_voltage = 5.0f, .codec_dac_voltage = 3.3f },
    });
    if (!s_codec) {
        ESP_LOGE(TAG, "es8311_codec_new 失败");
        return ESP_FAIL;
    }
    s_dev = esp_codec_dev_new(&(esp_codec_dev_cfg_t){
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
        .codec_if = s_codec,
        .data_if = s_data,   // 对齐小智：裸 data_if，不用包装层
    });
    if (!s_dev) {
        (void)audio_codec_delete_codec_if(s_codec);
        s_codec = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t bsp_audio_init(void)
{
    if (s_codec_release_failed) return ESP_ERR_INVALID_STATE;
    if (s_initialized) return ESP_OK;
    if (s_tx || s_rx || s_ctrl || s_data || s_codec || s_gpio || s_dev) {
        ESP_LOGE(TAG, "上次音频初始化回滚不完整，拒绝覆盖仍存活的资源句柄");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t e = bsp_i2c_init();
    if (e != ESP_OK) return e;

    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) { ESP_LOGE(TAG, "音频互斥量创建失败"); return ESP_ERR_NO_MEM; }
    }

    s_ctrl = audio_codec_new_i2c_ctrl(&(audio_codec_i2c_cfg_t){
        .port = BSP_I2C_PORT,
        .addr = BSP_I2C_ES8311_ADDR << 1,   // 该接口要 8 位地址形式（0x30，与小智一致）
        .bus_handle = bsp_i2c_bus(),
    });
    if (!s_ctrl) {
        ESP_LOGE(TAG, "ES8311 控制口创建失败 —— 用 bsp_i2c_scan() 确认 0x%02X 是否应答;"
                      "检查 SDA=GPIO%d / SCL=GPIO%d 接线与 codec 供电",
                 BSP_I2C_ES8311_ADDR, BSP_I2C_SDA, BSP_I2C_SCL);
        return ESP_FAIL;
    }

    if ((e = i2s_full_duplex_init()) != ESP_OK) goto fail;

    s_data = audio_codec_new_i2s_data(&(audio_codec_i2s_cfg_t){
        .port = BSP_I2S_PORT, .tx_handle = s_tx, .rx_handle = s_rx,
    });
    if (!s_data) { ESP_LOGE(TAG, "I2S 数据口创建失败"); e = ESP_ERR_NO_MEM; goto fail; }

    s_gpio = audio_codec_new_gpio();
    if (!s_gpio) { ESP_LOGE(TAG, "codec GPIO 接口创建失败"); e = ESP_ERR_NO_MEM; goto fail; }

    if ((e = audio_create_codec()) != ESP_OK) goto fail;

    s_initialized = true;
    ESP_LOGI(TAG, "ES8311 就绪（codec 常驻，按需 open）");
    return ESP_OK;

fail:
    if (s_dev) { esp_codec_dev_delete(s_dev); s_dev = NULL; }
    if (s_codec) { (void)audio_codec_delete_codec_if(s_codec); s_codec = NULL; }
    if (s_ctrl) (void)es8311_force_sleep();
    if (s_gpio) { audio_codec_delete_gpio_if(s_gpio); s_gpio = NULL; }
    if (s_data) { audio_codec_delete_data_if(s_data); s_data = NULL; }
    if (s_rx) { (void)i2s_channel_disable(s_rx); if (i2s_del_channel(s_rx) == ESP_OK) s_rx = NULL; }
    if (s_tx) { (void)i2s_channel_disable(s_tx); if (i2s_del_channel(s_tx) == ESP_OK) s_tx = NULL; }
    if (s_ctrl) { audio_codec_delete_ctrl_if(s_ctrl); s_ctrl = NULL; }
    return e;
}

esp_err_t bsp_audio_set_format(uint32_t hz, uint8_t bits, uint8_t ch)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (s_sleeping) return ESP_ERR_INVALID_STATE;

    audio_lock();

    /* 对齐小智 EnableInput/EnableOutput 的幂等语义：已打开且格式没变就直接复用。
     * 播放与录音同为 16000/16/1；第一次 open 时 esp_codec_dev 已把 TX/RX 两个方向
     * 一并配置并使能（dev_type=IN_OUT），所以这里的复用是"两个方向都已就绪"的复用，
     * 不是旧实现那种"录音沿用播放路径、输入方向从未被配置"的短路。 */
    if (s_opened && s_hz == hz && s_bits == bits && s_ch == ch) {
        audio_unlock();
        return ESP_OK;
    }

    esp_err_t e = ESP_FAIL;

    if (s_opened) {
        int r = esp_codec_dev_close(s_dev);
        if (r != ESP_CODEC_DEV_OK) ESP_LOGW(TAG, "esp_codec_dev_close 返回 %d", r);
        s_opened = false;
    }

    if ((e = audio_prepare_i2s_reopen()) != ESP_OK) goto fail;

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = bits,
        .channel = ch,
        /* 对齐小智固件（实测麦克风可用）：传 0 交给驱动决定收哪些槽；
         * 传 MAKE_CHANNEL_MASK(0)(=1) 会锁死在左槽。 */
        .channel_mask = 0,
        .sample_rate = hz,
        .mclk_multiple = 0,          // 0 → 驱动按默认 256xfs 取 MCLK
    };
    int r = esp_codec_dev_open(s_dev, &fs);
    if (r != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "esp_codec_dev_open 失败: %d", r);
        e = ESP_FAIL;
        goto fail;
    }

    // ⚠ open 之后【不要】手动覆写 ES8311 的时钟分频寄存器(REG01~06):
    //   驱动已按采样率与 MCLK 精确算好,覆写会导致 ADC/DAC 时序错乱、录音回放全是杂音。
    //   这里只设麦克风模拟 PGA 增益与输出音量。
    if ((r = esp_codec_dev_set_in_gain(s_dev, AUDIO_INPUT_GAIN_DB)) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "set_in_gain 失败: %d", r);
        e = ESP_FAIL;
        goto fail;
    }
    if ((r = esp_codec_dev_set_out_vol(s_dev, s_volume)) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "set_out_vol 失败: %d", r);
        e = ESP_FAIL;
        goto fail;
    }

    s_opened = true; s_hz = hz; s_bits = bits; s_ch = ch;
    ESP_LOGI(TAG, "codec 打开 %luHz/%ubit/%uch", (unsigned long)hz, bits, ch);
    es8311_log_regs("open");
    audio_unlock();
    return ESP_OK;

fail:
    // 半打开状态一律退到"未打开"，下次调用重新 open（codec 接口不销毁）。
    (void)esp_codec_dev_close(s_dev);
    s_opened = false;
    (void)audio_disable_i2s_channels();
    audio_unlock();
    return e;
}

esp_err_t bsp_audio_sleep(void)
{
    if (!s_initialized) return ESP_OK;
    if (s_sleeping) return s_sleep_result;

    audio_lock();
    esp_err_t first_error = ESP_OK;

    if (s_dev) {
        int r = esp_codec_dev_close(s_dev);
        if (r != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "休眠前 esp_codec_dev_close 返回 %d", r);
            first_error = ESP_FAIL;
        }
        s_opened = false;
    }

    // 驱动 close 会写它自带的较弱 suspend 序列，所以强制休眠必须放在最后。
    esp_err_t e = es8311_force_sleep();
    if (e != ESP_OK && first_error == ESP_OK) first_error = e;

    e = audio_disable_i2s_channels();
    if (e != ESP_OK && first_error == ESP_OK) first_error = e;
    s_sleeping = true;
    s_sleep_result = first_error;
    audio_unlock();
    return first_error;
}

esp_err_t bsp_audio_prepare_deep_sleep(void)
{
    esp_err_t first_error = audio_disable_i2s_channels();
    const int pins[] = {
        BSP_I2S_MCLK, BSP_I2S_BCLK, BSP_I2S_WS, BSP_I2S_DOUT, BSP_I2S_DIN,
    };
    uint64_t mask = 0;
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        if (pins[i] >= 0) mask |= 1ULL << (unsigned)pins[i];
    }

    gpio_config_t cfg = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t e = gpio_config(&cfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "I2S 引脚高阻配置失败: %s", esp_err_to_name(e));
        if (first_error == ESP_OK) first_error = e;
    } else {
        ESP_LOGI(TAG, "I2S MCLK/BCLK/WS/DOUT/DIN 已切换为高阻");
    }
    return first_error;
}

esp_err_t bsp_audio_wake(void)
{
    if (!s_initialized || !s_sleeping) return ESP_OK;

    audio_lock();
    s_sleeping = false;
    audio_unlock();

    esp_err_t e = bsp_audio_set_format(s_hz ? s_hz : AUDIO_DEFAULT_HZ,
                             s_bits ? s_bits : AUDIO_DEFAULT_BITS,
                             s_ch ? s_ch : AUDIO_DEFAULT_CH);
    if (e != ESP_OK) {
        s_sleeping = true;
        s_sleep_result = e;
        ESP_LOGE(TAG, "ES8311 唤醒失败: %s", esp_err_to_name(e));
        return e;
    }

    s_sleep_result = ESP_OK;
    ESP_LOGI(TAG, "ES8311 已从低功耗状态恢复");
    return ESP_OK;
}

esp_err_t bsp_audio_write(const void *pcm, size_t bytes)
{
    if (!s_dev || !s_opened || s_sleeping) return ESP_ERR_INVALID_STATE;
    return esp_codec_dev_write(s_dev, (void *)pcm, bytes) == 0 ? ESP_OK : ESP_FAIL;
}

// ⚠ 采集端分段统计（诊断用，不依赖 BLE）：
//   · 满量程恒定（min/max 恒 ±32768、mean(|x|) 不变）= 读到未初始化/错误位段；
//   · 对着麦克风说话时 mean(|x|) 应有 ≥1.5 倍起伏，否则就是没拾到音。
//   每 16 帧（512×16 ≈ 0.5s）输出一行，最多 520 行 —— 覆盖 main.c 里那 9 轮诊断窗口。
#define MIC_DIAG_MAX_SEGS 520U
#define MIC_DIAG_FRAMES_PER_SEG 16U
static unsigned  s_diag_seg;
static unsigned  s_diag_frames;
static unsigned  s_diag_n;
static int       s_diag_min;
static int       s_diag_max;
static long long s_diag_sum;

static void mic_seg_flush(void)
{
    if (s_diag_n == 0U) return;
    ESP_LOGI(TAG, "mic seg %2u: n=%4u min=%7d max=%7d mean(|x|)=%7d %s",
             s_diag_seg, s_diag_n, s_diag_min, s_diag_max,
             (int)(s_diag_sum / (long long)s_diag_n),
             (s_diag_max >= 32000 || s_diag_min <= -32000) ? "!!SATURATED(满量程)" : "");
    s_diag_seg++;
    s_diag_n = 0U;
    s_diag_min = 0;
    s_diag_max = 0;
    s_diag_sum = 0;
}

esp_err_t bsp_audio_read(void *pcm, size_t bytes)
{
    if (!s_dev || !s_opened || s_sleeping) return ESP_ERR_INVALID_STATE;
    int r = esp_codec_dev_read(s_dev, pcm, bytes);
    if (r == 0 && s_diag_seg < MIC_DIAG_MAX_SEGS && bytes >= sizeof(int16_t)) {
        const int16_t *s = (const int16_t *)pcm;
        size_t n = bytes / sizeof(int16_t);
        for (size_t i = 0; i < n; i++) {
            int v = s[i];
            if (s_diag_n == 0U) {
                s_diag_min = s_diag_max = v;
            } else {
                if (v < s_diag_min) s_diag_min = v;
                if (v > s_diag_max) s_diag_max = v;
            }
            s_diag_sum += (v < 0 ? -v : v);
            s_diag_n++;
        }
        if (++s_diag_frames >= MIC_DIAG_FRAMES_PER_SEG) {
            s_diag_frames = 0U;
            mic_seg_flush();
        }
    }
    return r == 0 ? ESP_OK : ESP_FAIL;
}

void bsp_audio_set_volume(uint8_t percent)
{
    audio_lock();
    s_volume = percent > 100 ? 100 : percent;
    if (s_dev && s_opened && !s_sleeping) esp_codec_dev_set_out_vol(s_dev, s_volume);
    audio_unlock();
}
