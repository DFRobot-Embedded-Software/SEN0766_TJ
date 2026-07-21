#include <stdio.h>
#include <time.h>

#include "esp_log.h"
#include "esp_sntp.h"
#include "mdns.h"
#include "mcp_server.h"
#include "sensor_data.h"
#include "web_fs.h"
#include "web_server.h"
#include "wifi_manager.h"

static const char *TAG = "mcp_sensors";

static void init_time_sync(void)
{
    setenv("TZ", "CST-8", 1);
    tzset();

    if (esp_sntp_enabled()) {
        return;
    }

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
    ESP_LOGI(TAG, "SNTP started (CST-8) for event timestamps");
}

static void on_wifi_connected(const char *ip_addr)
{
    ESP_LOGI(TAG, "WiFi connected:");
    ESP_LOGI(TAG, "  Web UI: http://%s or http://mcp-sensors.local", ip_addr);
    ESP_LOGI(TAG, "  MCP:    http://%s:%d%s", ip_addr, MCP_HTTP_PORT, MCP_ENDPOINT_URI);

    mdns_init();
    mdns_hostname_set("mcp-sensors");
    mdns_instance_name_set("MCP Sensors");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);

    init_time_sync();

    web_server_stop();
    mcp_server_stop();

    /* MCP 先启动，避免 Web httpd 占满资源导致第二个 httpd 绑定 8080 失败 */
    esp_err_t err = mcp_server_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MCP server start failed: %s", esp_err_to_name(err));
        return;
    }

    err = web_server_start_dashboard();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "web server start failed: %s", esp_err_to_name(err));
        mcp_server_stop();
        return;
    }

    err = web_server_register_static_routes();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "static routes register failed: %s", esp_err_to_name(err));
    }
}

static void on_ap_started(void)
{
    mcp_server_stop();
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
