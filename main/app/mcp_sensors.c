#include <stdio.h>
#include <time.h>

#include "esp_log.h"
#include "sdkconfig.h"
#if !CONFIG_YAMNET_VERIFY_ONLY
#include "esp_sntp.h"
#include "mdns.h"
#include "mcp_server.h"
#include "sensor_data.h"
#include "web_fs.h"
#include "web_server.h"
#include "wifi_manager.h"
#endif
#if CONFIG_YAMNET_ENABLE
#include "yamnet_classifier.h"
#if CONFIG_AUDIO_STREAM_ENABLE
#include "audio_stream.h"
#endif
#endif

static const char *TAG = "mcp_sensors";

#if !CONFIG_YAMNET_VERIFY_ONLY

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

#endif /* !YAMNET_VERIFY_ONLY */

void app_main(void)
{
#if CONFIG_YAMNET_VERIFY_ONLY
    ESP_LOGI(TAG, "YAMNet verify-only mode (no WiFi / sensors / MCP / Web)");

#if CONFIG_YAMNET_ENABLE
    esp_err_t err = yamnet_classifier_start(NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "YAMNet start failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "YAMNet running — watch serial for infer logs (pcm_rms / mel / Top-3)");
#else
    ESP_LOGE(TAG, "Enable CONFIG_YAMNET_ENABLE in menuconfig");
#endif

#else /* full application */

    ESP_LOGI(TAG, "MCP Sensors starting...");

    esp_err_t err = nvs_storage_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(err));
        return;
    }

    sensor_data_init();

#if CONFIG_YAMNET_ENABLE
    err = yamnet_classifier_start(NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "YAMNet start failed: %s (check model partition / PSRAM)", esp_err_to_name(err));
    }
#if CONFIG_AUDIO_STREAM_ENABLE
    err = audio_stream_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "audio_stream init failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "audio_stream ready (call audio_stream_start on wake)");
    }
#endif
#endif

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

#endif /* YAMNET_VERIFY_ONLY */
}
