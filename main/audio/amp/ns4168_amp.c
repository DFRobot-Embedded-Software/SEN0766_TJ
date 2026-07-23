/**
 * @file ns4168_amp.c
 * @brief NS4168：I2S TX 播放
 */
#include "ns4168_amp.h"

#include "esp_log.h"
#include "sdkconfig.h"

#if CONFIG_NS4168_ENABLE
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#endif

static const char *TAG = "ns4168";

#if CONFIG_NS4168_ENABLE

static i2s_chan_handle_t s_i2s_tx;
static bool s_ready;
static bool s_enabled;

esp_err_t ns4168_amp_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG((i2s_port_t)CONFIG_NS4168_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;

    esp_err_t err = i2s_new_channel(&chan_cfg, &s_i2s_tx, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel TX failed: %s", esp_err_to_name(err));
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(CONFIG_NS4168_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = CONFIG_NS4168_I2S_BCLK,
            .ws = CONFIG_NS4168_I2S_WS,
            .dout = CONFIG_NS4168_I2S_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

    err = i2s_channel_init_std_mode(s_i2s_tx, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S TX init failed: %s", esp_err_to_name(err));
        i2s_del_channel(s_i2s_tx);
        s_i2s_tx = NULL;
        return err;
    }

    err = i2s_channel_enable(s_i2s_tx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S TX enable failed: %s", esp_err_to_name(err));
        i2s_del_channel(s_i2s_tx);
        s_i2s_tx = NULL;
        return err;
    }

    s_ready = true;
    s_enabled = true;
    ESP_LOGI(TAG,
             "NS4168 ready @ %d Hz I2S%u BCLK=%d WS=%d DOUT=%d",
             CONFIG_NS4168_SAMPLE_RATE,
             (unsigned)CONFIG_NS4168_I2S_PORT,
             CONFIG_NS4168_I2S_BCLK,
             CONFIG_NS4168_I2S_WS,
             CONFIG_NS4168_I2S_DOUT);
    return ESP_OK;
}

void ns4168_amp_deinit(void)
{
    if (s_i2s_tx != NULL) {
        i2s_channel_disable(s_i2s_tx);
        i2s_del_channel(s_i2s_tx);
        s_i2s_tx = NULL;
    }
    s_ready = false;
    s_enabled = false;
}

bool ns4168_amp_is_ready(void)
{
    return s_ready;
}

esp_err_t ns4168_amp_set_enabled(bool enabled)
{
    if (!s_ready || s_i2s_tx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (enabled == s_enabled) {
        return ESP_OK;
    }

    esp_err_t err;
    if (enabled) {
        err = i2s_channel_enable(s_i2s_tx);
    } else {
        err = i2s_channel_disable(s_i2s_tx);
    }
    if (err != ESP_OK) {
        return err;
    }
    s_enabled = enabled;
    return ESP_OK;
}

esp_err_t ns4168_amp_write(const int16_t *pcm, size_t n_samples)
{
    if (!s_ready || !s_enabled || s_i2s_tx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (pcm == NULL || n_samples == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t bytes = n_samples * sizeof(int16_t);
    size_t written = 0;
    esp_err_t err = i2s_channel_write(s_i2s_tx, pcm, bytes, &written, pdMS_TO_TICKS(1000));
    if (err != ESP_OK) {
        return err;
    }
    if (written != bytes) {
        ESP_LOGW(TAG, "short write %u/%u", (unsigned)written, (unsigned)bytes);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

#else /* !CONFIG_NS4168_ENABLE */

esp_err_t ns4168_amp_init(void)
{
    return ESP_OK;
}

void ns4168_amp_deinit(void) {}

bool ns4168_amp_is_ready(void)
{
    return false;
}

esp_err_t ns4168_amp_write(const int16_t *pcm, size_t n_samples)
{
    (void)pcm;
    (void)n_samples;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ns4168_amp_set_enabled(bool enabled)
{
    (void)enabled;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif /* CONFIG_NS4168_ENABLE */
