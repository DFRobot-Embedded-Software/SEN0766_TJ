#include <stdio.h>

#include "esp_log.h"
#include "mdns.h"
#include "mdns.h"
#include "sensor_data.h"
#include "web_fs.h"
#include "web_server.h"
#include "wifi_manager.h"

static const char *TAG = "mcp_sensors";

static void on_wifi_connected(const char *ip_addr)
{
    ESP_LOGI(TAG, "WiFi connected, open http://%s or http://mcp-sensors.local", ip_addr);

    mdns_init();
    mdns_hostname_set("mcp-sensors");
    mdns_instance_name_set("MCP Sensors");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);

    web_server_stop();
    web_server_start_dashboard();
}

static void on_ap_started(void)
{
    mdns_free();
    web_server_stop();
    web_server_start_provision();
}

void app_main(void)
{
    ESP_LOGI(TAG, "MCP Sensors starting...");

    esp_err_t err = nvs_storage_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(err));
        return;
    }

    sensor_data_init();

    err = web_fs_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "web fs init failed: %s", esp_err_to_name(err));
        return;
    }

    err = wifi_manager_init(on_wifi_connected, on_ap_started);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi init failed: %s", esp_err_to_name(err));
        return;
    }

    if (!wifi_manager_is_configured()) {
        web_server_start_provision();
    }
}
