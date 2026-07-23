/**
 * @file audio_capture.c
 * @brief 独立采集实现：ES7210/I2S、数字增益、引用计数、多 sink 扇出
 */
#include "audio_capture.h"

#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if CONFIG_AUDIO_SOURCE_ES7210
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "es7210_adc.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#endif

static const char *TAG = "audio_capture";

#ifndef CONFIG_AUDIO_CAPTURE_DIGITAL_GAIN
#ifdef CONFIG_YAMNET_DIGITAL_GAIN
#define CONFIG_AUDIO_CAPTURE_DIGITAL_GAIN CONFIG_YAMNET_DIGITAL_GAIN
#else
#define CONFIG_AUDIO_CAPTURE_DIGITAL_GAIN 1
#endif
#endif

typedef struct {
    audio_pcm_sink_fn fn;
    void *ctx;
    bool used;
} sink_slot_t;

static SemaphoreHandle_t s_lock;
static int s_refcount;
static TaskHandle_t s_task;
static volatile bool s_running;
static sink_slot_t s_sinks[AUDIO_CAPTURE_MAX_SINKS];

#if CONFIG_AUDIO_SOURCE_ES7210
static i2s_chan_handle_t s_i2s_rx;
static i2c_master_bus_handle_t s_i2c_owned;
static const audio_codec_data_if_t *s_data_if;
static const audio_codec_ctrl_if_t *s_ctrl_if;
static const audio_codec_gpio_if_t *s_gpio_if;
static const audio_codec_if_t *s_codec_if;
static esp_codec_dev_handle_t s_record_dev;
static int s_hw_channels = 1;
#endif

static void ensure_lock(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
}

/** �?int16 PCM 做固定倍数放大并饱和削波�?*/
static void apply_digital_gain(int16_t *buf, size_t n_samples)
{
#if CONFIG_AUDIO_CAPTURE_DIGITAL_GAIN > 1
    const int32_t g = (int32_t)CONFIG_AUDIO_CAPTURE_DIGITAL_GAIN;
    for (size_t i = 0; i < n_samples; i++) {
        int32_t v = (int32_t)buf[i] * g;
        if (v > 32767) {
            v = 32767;
        } else if (v < -32768) {
            v = -32768;
        }
        buf[i] = (int16_t)v;
    }
#else
    (void)buf;
    (void)n_samples;
#endif
}

/** 将一�?PCM 分发给所有已注册 sink（拷贝表后调用，避免长时间持锁）�?*/
static void fanout_pcm(const int16_t *buf, size_t n_samples)
{
    sink_slot_t local[AUDIO_CAPTURE_MAX_SINKS];

    if (s_lock != NULL && xSemaphoreTake(s_lock, pdMS_TO_TICKS(5)) == pdTRUE) {
        memcpy(local, s_sinks, sizeof(local));
        xSemaphoreGive(s_lock);
    } else {
        memcpy(local, s_sinks, sizeof(local));
    }

    for (int i = 0; i < AUDIO_CAPTURE_MAX_SINKS; i++) {
        if (local[i].used && local[i].fn != NULL) {
            local[i].fn(buf, n_samples, local[i].ctx);
        }
    }
}

#if CONFIG_AUDIO_SOURCE_ES7210
/** 立体声交�?�?单声道（平均）原地压缩�?*/
static size_t stereo_to_mono(int16_t *buf, size_t n_interleaved)
{
    size_t frames = n_interleaved / 2;
    for (size_t i = 0; i < frames; i++) {
        int32_t l = buf[i * 2];
        int32_t r = buf[i * 2 + 1];
        buf[i] = (int16_t)((l + r) / 2);
    }
    return frames;
}
#endif

/** 采集任务：读 I2S（或假数据）�?数字增益 �?fanout�?*/
static void capture_task(void *arg)
{
    (void)arg;
    int16_t buf[512];

    while (s_running) {
#if CONFIG_AUDIO_SOURCE_ES7210
        if (s_record_dev != NULL) {
            int ret = esp_codec_dev_read(s_record_dev, buf, (int)sizeof(buf));
            if (ret == ESP_CODEC_DEV_OK) {
                size_t n = sizeof(buf) / sizeof(buf[0]);
                if (s_hw_channels >= 2) {
                    n = stereo_to_mono(buf, n);
                }
                apply_digital_gain(buf, n);
                fanout_pcm(buf, n);
            } else {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
#else
        for (int i = 0; i < 320; i++) {
            buf[i] = (int16_t)((esp_random() & 0x3FF) - 512);
        }
        apply_digital_gain(buf, 320);
        fanout_pcm(buf, 320);
        vTaskDelay(pdMS_TO_TICKS(20));
#endif
    }

    s_task = NULL;
    vTaskDelete(NULL);
}

#if CONFIG_AUDIO_SOURCE_ES7210

static esp_err_t ensure_i2c_bus(i2c_master_bus_handle_t *out_bus)
{
    i2c_port_t port = (i2c_port_t)CONFIG_ES7210_I2C_PORT;
    int sda = CONFIG_ES7210_I2C_SDA;
    int scl = CONFIG_ES7210_I2C_SCL;
    ESP_LOGI(TAG, "ES7210 I2C%u SDA=%d SCL=%d", (unsigned)port, sda, scl);

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = port,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_i2c_owned);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus(%d) failed: %s", (int)port, esp_err_to_name(err));
        return err;
    }
    *out_bus = s_i2c_owned;
    return ESP_OK;
}

static uint8_t es7210_addr_for_codec(uint8_t addr_7bit)
{
    if (addr_7bit == 0x40) {
        return ES7210_CODEC_DEFAULT_ADDR; /* 0x80 */
    }
    return (uint8_t)(addr_7bit << 1);
}

static esp_err_t probe_es7210(i2c_master_bus_handle_t bus, uint8_t *addr_out)
{
    const uint8_t candidates[] = {
        (uint8_t)CONFIG_ES7210_I2C_ADDR,
        0x40,
        0x41,
        0x42,
        0x43,
    };

    for (size_t i = 0; i < sizeof(candidates); i++) {
        uint8_t addr = candidates[i];
        bool seen = false;
        for (size_t j = 0; j < i; j++) {
            if (candidates[j] == addr) {
                seen = true;
                break;
            }
        }
        if (seen) {
            continue;
        }
        if (i2c_master_probe(bus, addr, 200) == ESP_OK) {
            *addr_out = addr;
            ESP_LOGI(TAG, "ES7210 ACK at 7-bit addr 0x%02X", addr);
            return ESP_OK;
        }
        ESP_LOGW(TAG, "no ACK at 0x%02X", addr);
    }
    return ESP_ERR_NOT_FOUND;
}

static uint8_t es7210_mic_mask(void)
{
    uint8_t m = ES7210_SEL_MIC1;
#if CONFIG_ES7210_MIC_COUNT >= 2
    m |= ES7210_SEL_MIC2;
#endif
    return m;
}

static esp_err_t start_es7210(void)
{
    i2c_master_bus_handle_t i2c_bus = NULL;
    esp_err_t err = ensure_i2c_bus(&i2c_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus failed: %s", esp_err_to_name(err));
        return err;
    }

    uint8_t es_addr = (uint8_t)CONFIG_ES7210_I2C_ADDR;
    err = probe_es7210(i2c_bus, &es_addr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "ES7210 not found on I2C (tried 0x40..). "
                 "Check SDA/SCL/ADDR and menuconfig ES7210_I2C_*");
        return err;
    }

    s_hw_channels = CONFIG_ES7210_MIC_COUNT;

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;

    /* ES7210 �?ADC：只�?RX */
    err = i2s_new_channel(&chan_cfg, NULL, &s_i2s_rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel RX failed: %s", esp_err_to_name(err));
        return err;
    }

    i2s_slot_mode_t slot_mode =
        (s_hw_channels >= 2) ? I2S_SLOT_MODE_STEREO : I2S_SLOT_MODE_MONO;

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_CAPTURE_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, slot_mode),
        .gpio_cfg = {
#if CONFIG_ES7210_I2S_USE_MCLK
            .mclk = CONFIG_ES7210_I2S_MCLK,
#else
            .mclk = I2S_GPIO_UNUSED,
#endif
            .bclk = CONFIG_ES7210_I2S_BCLK,
            .ws = CONFIG_ES7210_I2S_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = CONFIG_ES7210_I2S_DIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
#if CONFIG_ES7210_I2S_USE_MCLK
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
#endif
    if (s_hw_channels < 2) {
        std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    }

    err = i2s_channel_init_std_mode(s_i2s_rx, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S RX init failed: %s", esp_err_to_name(err));
        return err;
    }
    err = i2s_channel_enable(s_i2s_rx);
    if (err != ESP_OK) {
        return err;
    }

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = s_i2s_rx,
        .tx_handle = NULL,
    };
    s_data_if = audio_codec_new_i2s_data(&i2s_cfg);
    if (s_data_if == NULL) {
        return ESP_FAIL;
    }

    const uint8_t es_addr_codec = es7210_addr_for_codec(es_addr);
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = (i2c_port_t)CONFIG_ES7210_I2C_PORT,
        .addr = es_addr_codec,
        .bus_handle = i2c_bus,
    };
    s_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (s_ctrl_if == NULL) {
        return ESP_FAIL;
    }

    s_gpio_if = audio_codec_new_gpio();
    if (s_gpio_if == NULL) {
        return ESP_FAIL;
    }

    es7210_codec_cfg_t es_cfg = {
        .ctrl_if = s_ctrl_if,
        .master_mode = false,
        .mic_selected = es7210_mic_mask(),
        .mclk_src = ES7210_MCLK_FROM_PAD,
        .mclk_div = 256,
    };
    s_codec_if = es7210_codec_new(&es_cfg);
    if (s_codec_if == NULL) {
        ESP_LOGE(TAG, "es7210_codec_new failed");
        return ESP_FAIL;
    }

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = s_codec_if,
        .data_if = s_data_if,
    };
    s_record_dev = esp_codec_dev_new(&dev_cfg);
    if (s_record_dev == NULL) {
        ESP_LOGE(TAG, "esp_codec_dev_new failed");
        return ESP_FAIL;
    }

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = (uint8_t)s_hw_channels,
        .channel_mask = (s_hw_channels >= 2)
                            ? (ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) | ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1))
                            : ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0),
        .sample_rate = AUDIO_CAPTURE_SAMPLE_RATE,
        .mclk_multiple = 0,
    };
    err = esp_codec_dev_open(s_record_dev, &fs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_codec_dev_open failed: %s", esp_err_to_name(err));
        return err;
    }

    (void)esp_codec_dev_set_in_gain(s_record_dev, (float)CONFIG_ES7210_MIC_GAIN_DB);

    ESP_LOGI(TAG,
             "ES7210 ready @ %d Hz ch=%d mic_hw=%ddB dig_gain=x%d "
             "(I2C 7-bit=0x%02X codec=0x%02X BCLK=%d WS=%d DIN=%d MCLK=%s)",
             AUDIO_CAPTURE_SAMPLE_RATE,
             s_hw_channels,
             CONFIG_ES7210_MIC_GAIN_DB,
             CONFIG_AUDIO_CAPTURE_DIGITAL_GAIN,
             es_addr,
             es_addr_codec,
             CONFIG_ES7210_I2S_BCLK,
             CONFIG_ES7210_I2S_WS,
             CONFIG_ES7210_I2S_DIN,
#if CONFIG_ES7210_I2S_USE_MCLK
             "on"
#else
             "off"
#endif
    );
    return ESP_OK;
}

static void stop_es7210(void)
{
    if (s_record_dev != NULL) {
        esp_codec_dev_close(s_record_dev);
        esp_codec_dev_delete(s_record_dev);
        s_record_dev = NULL;
    }
    if (s_codec_if != NULL) {
        audio_codec_delete_codec_if(s_codec_if);
        s_codec_if = NULL;
    }
    if (s_ctrl_if != NULL) {
        audio_codec_delete_ctrl_if(s_ctrl_if);
        s_ctrl_if = NULL;
    }
    if (s_data_if != NULL) {
        audio_codec_delete_data_if(s_data_if);
        s_data_if = NULL;
    }
    if (s_gpio_if != NULL) {
        audio_codec_delete_gpio_if(s_gpio_if);
        s_gpio_if = NULL;
    }
    if (s_i2s_rx != NULL) {
        i2s_channel_disable(s_i2s_rx);
        i2s_del_channel(s_i2s_rx);
        s_i2s_rx = NULL;
    }
    if (s_i2c_owned != NULL) {
        i2c_del_master_bus(s_i2c_owned);
        s_i2c_owned = NULL;
    }
}
#endif /* ES7210 */

/** 打开硬件并拉起采集任务（�?refcount �?0�? 时调用）�?*/
static esp_err_t start_hw(void)
{
#if CONFIG_AUDIO_SOURCE_ES7210
    esp_err_t err = start_es7210();
    if (err != ESP_OK) {
        stop_es7210();
        return err;
    }
#else
    ESP_LOGW(TAG, "capture = fake noise (select ES7210); dig_gain=x%d",
             CONFIG_AUDIO_CAPTURE_DIGITAL_GAIN);
#endif

    s_running = true;
    if (xTaskCreate(capture_task, "aud_cap", 4096, NULL, 5, &s_task) != pdPASS) {
        s_running = false;
#if CONFIG_AUDIO_SOURCE_ES7210
        stop_es7210();
#endif
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/** 停止采集任务并关闭硬件�?*/
static void stop_hw(void)
{
    s_running = false;
    while (s_task != NULL) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
#if CONFIG_AUDIO_SOURCE_ES7210
    stop_es7210();
#endif
}

esp_err_t audio_capture_acquire(void)
{
    ensure_lock();
    xSemaphoreTake(s_lock, portMAX_DELAY);

    if (s_refcount == 0) {
        esp_err_t err = start_hw();
        if (err != ESP_OK) {
            xSemaphoreGive(s_lock);
            return err;
        }
    }
    s_refcount++;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "acquire ref=%d", s_refcount);
    return ESP_OK;
}

void audio_capture_release(void)
{
    ensure_lock();
    xSemaphoreTake(s_lock, portMAX_DELAY);

    if (s_refcount <= 0) {
        s_refcount = 0;
        xSemaphoreGive(s_lock);
        return;
    }
    s_refcount--;
    if (s_refcount == 0) {
        xSemaphoreGive(s_lock);
        stop_hw();
        ESP_LOGI(TAG, "release ref=0 (HW stopped)");
        return;
    }
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "release ref=%d", s_refcount);
}

esp_err_t audio_capture_add_sink(audio_pcm_sink_fn fn, void *ctx, int *out_id)
{
    if (fn == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ensure_lock();
    xSemaphoreTake(s_lock, portMAX_DELAY);

    for (int i = 0; i < AUDIO_CAPTURE_MAX_SINKS; i++) {
        if (!s_sinks[i].used) {
            s_sinks[i].fn = fn;
            s_sinks[i].ctx = ctx;
            s_sinks[i].used = true;
            if (out_id != NULL) {
                *out_id = i;
            }
            xSemaphoreGive(s_lock);
            ESP_LOGI(TAG, "sink %d registered", i);
            return ESP_OK;
        }
    }

    xSemaphoreGive(s_lock);
    return ESP_ERR_NO_MEM;
}

void audio_capture_remove_sink(int id)
{
    if (id < 0 || id >= AUDIO_CAPTURE_MAX_SINKS) {
        return;
    }
    ensure_lock();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_sinks[id].fn = NULL;
    s_sinks[id].ctx = NULL;
    s_sinks[id].used = false;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "sink %d removed", id);
}

uint32_t audio_capture_sample_rate(void)
{
    return AUDIO_CAPTURE_SAMPLE_RATE;
}
