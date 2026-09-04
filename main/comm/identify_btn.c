/**
 * @file identify_btn.c
 * @brief 长按开临时 AP，门户显示 STA IP / id / .local。天花板安装不靠串口。
 */
#include "identify_btn.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "web_server.h"
#include "wifi_manager.h"

static const char *TAG = "identify";

#if CONFIG_COMM_IDENTIFY_BTN_ENABLE

static void btn_task(void *arg)
{
    (void)arg;
    const int pin = CONFIG_COMM_IDENTIFY_GPIO;
    const int held_ms = CONFIG_COMM_IDENTIFY_HOLD_MS;
    const int timeout_ms = CONFIG_COMM_IDENTIFY_AP_S * 1000;
    int down_ms = 0;
    int ap_left = 0;

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = CONFIG_COMM_IDENTIFY_ACTIVE_LOW ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = CONFIG_COMM_IDENTIFY_ACTIVE_LOW ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    while (1) {
        int level = gpio_get_level(pin);
        bool pressed = CONFIG_COMM_IDENTIFY_ACTIVE_LOW ? (level == 0) : (level != 0);
        if (pressed) {
            down_ms += 50;
            if (down_ms >= held_ms) {
                down_ms = 0;
                if (wifi_manager_is_identify_ap()) {
                    wifi_manager_stop_identify_ap();
                    ap_left = 0;
                } else {
                    if (wifi_manager_start_identify_ap() == ESP_OK) {
                        ap_left = timeout_ms;
                    }
                }
                while (CONFIG_COMM_IDENTIFY_ACTIVE_LOW ? (gpio_get_level(pin) == 0)
                                                       : (gpio_get_level(pin) != 0)) {
                    vTaskDelay(pdMS_TO_TICKS(50));
                }
            }
        } else {
            down_ms = 0;
        }
        if (ap_left > 0) {
            ap_left -= 50;
            if (ap_left <= 0) {
                wifi_manager_stop_identify_ap();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

esp_err_t identify_btn_start(void)
{
    if (xTaskCreate(btn_task, "identify", 3072, NULL, 3, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "long-press GPIO%d %dms -> identify AP", CONFIG_COMM_IDENTIFY_GPIO,
             CONFIG_COMM_IDENTIFY_HOLD_MS);
    return ESP_OK;
}

#else

esp_err_t identify_btn_start(void)
{
    ESP_LOGI(TAG, "identify button disabled (menuconfig COMM_IDENTIFY_BTN_ENABLE)");
    return ESP_OK;
}

#endif
