/**
 * @file ws2812_led.c
 * @brief WS2812 灯带：led_strip + RMT
 */
#include "ws2812_led.h"

#include "esp_log.h"
#include "sdkconfig.h"

#if CONFIG_WS2812_ENABLE
#include "led_strip.h"
#endif

static const char *TAG = "ws2812";

#if CONFIG_WS2812_ENABLE

static led_strip_handle_t s_strip;

esp_err_t ws2812_led_init(void)
{
    if (s_strip != NULL) {
        return ESP_OK;
    }

    led_strip_config_t strip_cfg = {
        .strip_gpio_num = CONFIG_WS2812_GPIO,
        .max_leds = CONFIG_WS2812_LED_COUNT,
        .led_model = LED_MODEL_WS2812,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .flags = {
            .invert_out = false,
        },
    };

    led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .flags = {
            .with_dma = false,
        },
    };

    esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "led_strip_new_rmt_device failed: %s", esp_err_to_name(err));
        s_strip = NULL;
        return err;
    }

    ESP_LOGI(TAG, "WS2812 ready GPIO=%d leds=%d", CONFIG_WS2812_GPIO, CONFIG_WS2812_LED_COUNT);
    return ws2812_led_clear();
}

void ws2812_led_deinit(void)
{
    if (s_strip == NULL) {
        return;
    }
    (void)led_strip_clear(s_strip);
    (void)led_strip_del(s_strip);
    s_strip = NULL;
}

esp_err_t ws2812_led_set_pixel(uint32_t index, uint8_t r, uint8_t g, uint8_t b)
{
    if (s_strip == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (index >= (uint32_t)CONFIG_WS2812_LED_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    return led_strip_set_pixel(s_strip, index, r, g, b);
}

esp_err_t ws2812_led_refresh(void)
{
    if (s_strip == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return led_strip_refresh(s_strip);
}

esp_err_t ws2812_led_set_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    if (s_strip == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    for (int i = 0; i < CONFIG_WS2812_LED_COUNT; i++) {
        esp_err_t err = led_strip_set_pixel(s_strip, i, r, g, b);
        if (err != ESP_OK) {
            return err;
        }
    }
    return led_strip_refresh(s_strip);
}

esp_err_t ws2812_led_clear(void)
{
    if (s_strip == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return led_strip_clear(s_strip);
}

#else /* !CONFIG_WS2812_ENABLE */

esp_err_t ws2812_led_init(void)
{
    return ESP_OK;
}

void ws2812_led_deinit(void) {}

esp_err_t ws2812_led_set_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    (void)r;
    (void)g;
    (void)b;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ws2812_led_set_pixel(uint32_t index, uint8_t r, uint8_t g, uint8_t b)
{
    (void)index;
    (void)r;
    (void)g;
    (void)b;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ws2812_led_refresh(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ws2812_led_clear(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

#endif /* CONFIG_WS2812_ENABLE */
