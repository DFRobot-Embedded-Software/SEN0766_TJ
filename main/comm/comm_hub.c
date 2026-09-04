/**
 * @file comm_hub.c
 * @brief 枢纽：mDNS + CoAP + MQTT 长连接 + 语音发现。
 *        叶子 CONFIG_COMM_ROLE_LEAF：只在 emit 时 MQTT，不启 CoAP/mDNS/WS。
 */
#include "comm_hub.h"

#include "coap_hub.h"
#include "esp_log.h"
#include "mdns_hub.h"
#include "mqtt_hub.h"
#include "sdkconfig.h"
#include "voice_discover.h"

static const char *TAG = "comm_hub";

esp_err_t comm_hub_start(void)
{
#if CONFIG_COMM_ROLE_LEAF
    ESP_LOGI(TAG, "leaf: skip CoAP/mDNS/WS; MQTT on event publish");
    return ESP_OK;
#else
    esp_err_t err = mdns_hub_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mdns: %s", esp_err_to_name(err));
    }
    err = coap_hub_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "coap: %s", esp_err_to_name(err));
        return err;
    }
    err = mqtt_hub_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mqtt: %s", esp_err_to_name(err));
    }
    err = voice_discover_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "voice discover: %s", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "hub up: CoAP:5683 MQTT events mDNS _mcp-sensors");
    return ESP_OK;
#endif
}

void comm_hub_stop(void)
{
#if !CONFIG_COMM_ROLE_LEAF
    voice_discover_stop();
    mqtt_hub_stop();
    coap_hub_stop();
    mdns_hub_stop();
#endif
}
