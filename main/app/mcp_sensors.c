#include <stdio.h>
#include <time.h>

#include "esp_log.h"
#include "sdkconfig.h"
#include "ns4168_amp.h"
#include "ws2812_led.h"
#include "rgb_ui.h"

static const char *TAG = "mcp_sensors";

#if !CONFIG_YAMNET_VERIFY_ONLY
#include "esp_sntp.h"
#include "board_snapshot.h"
#include "capture_session.h"
#include "comm_hub.h"
#include "device_id.h"
#include "identify_btn.h"
#include "ota_update.h"
#include "web_fs.h"
#include "web_log.h"
#include "web_server.h"
#include "wifi_manager.h"
#endif
#if CONFIG_YAMNET_ENABLE
#include "yamnet_classifier.h"
#if CONFIG_AUDIO_STREAM_ENABLE
#include "audio_stream.h"
#endif
#if CONFIG_AUDIO_WS_ENABLE
#include "audio_ws_client.h"
#endif
#if CONFIG_ESP_SR_WAKE_ENABLE
#include "esp_sr_wake.h"
#endif
#endif
#if CONFIG_ENV_SOUND_ENABLE
#include "env_sound.h"
#endif

#if !CONFIG_YAMNET_VERIFY_ONLY

static void on_wifi_connecting(void)
{
    rgb_ui_set_net(RGB_NET_CONNECTING);
}

static void on_wifi_connect_failed(void)
{
    rgb_ui_set_net(RGB_NET_FAIL);
}

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

static bool s_sta_services;

static void on_wifi_connected(const char *ip_addr)
{
    char id[8] = {0};
    char host[32] = "mcp-sensors";
    (void)device_id_get(id, sizeof(id));
    if (id[0]) {
        snprintf(host, sizeof(host), "mcp-sensors-%s", id);
    }

    ESP_LOGI(TAG, "boot: phase6 online");
    ESP_LOGI(TAG, "WiFi connected:");
    ESP_LOGI(TAG, "  Web UI: http://%s or http://%s.local", ip_addr, host);
    ESP_LOGI(TAG, "  CoAP:   udp://%s:5683", ip_addr);
    ESP_LOGI(TAG, "  device_id: %s", id[0] ? id : "(unknown)");

    rgb_ui_set_net(RGB_NET_OK);

    /* 不要每次 GOT_IP 都拆 comm_hub：MQTT wait 任务会被停掉且可能起不回来，
     * 事件卡住直到 App 重启再下发 broker。Wi‑Fi 闪断只让 MQTT 客户端自重连。 */
    if (!s_sta_services) {
        esp_err_t err = comm_hub_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "comm hub start failed: %s", esp_err_to_name(err));
        } else {
            s_sta_services = true;
        }
    }
    init_time_sync();

    web_server_stop();

#if !CONFIG_COMM_ROLE_LEAF
    esp_err_t err = web_server_start_dashboard();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "web server start failed: %s", esp_err_to_name(err));
    } else {
        err = web_server_register_static_routes();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "static routes register failed: %s", esp_err_to_name(err));
        }
    }
#else
    ESP_LOGI(TAG, "leaf: no STA dashboard; identify AP still available");
#endif

    ota_update_mark_valid();
}

static void on_ap_started(void)
{
    if (wifi_manager_is_identify_ap()) {
        return;
    }
    if (rgb_ui_get_net() != RGB_NET_FAIL) {
        rgb_ui_set_net(RGB_NET_AP_WAIT);
    }
    comm_hub_stop();
    s_sta_services = false;
    web_server_stop();
    web_server_start_provision();
}

static esp_err_t boot_platform(void)
{
    ESP_LOGI(TAG, "boot: phase0 platform");
    return nvs_storage_init();
}

static void boot_ui(void)
{
    ESP_LOGI(TAG, "boot: phase1 ui");
    esp_err_t err = ws2812_led_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WS2812 init failed: %s", esp_err_to_name(err));
    } else {
        (void)rgb_ui_init();
        rgb_ui_set_net(RGB_NET_SELFTEST);
    }

    err = ns4168_amp_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NS4168 init failed: %s", esp_err_to_name(err));
    }
}

static void boot_sensors(void)
{
    ESP_LOGI(TAG, "boot: phase2 sensors");
    board_snapshot_init();
}

static esp_err_t boot_storage(void)
{
    ESP_LOGI(TAG, "boot: phase3 storage");
    return web_fs_init();
}

static esp_err_t boot_network(void)
{
    ESP_LOGI(TAG, "boot: phase4 network");
    /* 须在 init 自动连网之前注册，否则会错过 CONNECTING 灯效 */
    wifi_manager_set_progress_cbs(on_wifi_connecting, on_wifi_connect_failed);
    esp_err_t err = wifi_manager_init(on_wifi_connected, on_ap_started);
    if (err != ESP_OK) {
        return err;
    }
    if (!wifi_manager_is_configured()) {
        web_server_start_provision();
    }
    return ESP_OK;
}

#endif /* !YAMNET_VERIFY_ONLY */

#if CONFIG_YAMNET_ENABLE
static void on_wakeword(void *ctx)
{
    (void)ctx;
    /* 灯效由 audio_ws：uplink→识别、processing→处理、play→播放 */
}

static void boot_audio(void)
{
    ESP_LOGI(TAG, "boot: phase5 audio");
    esp_err_t err = yamnet_classifier_start(NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "YAMNet start failed: %s", esp_err_to_name(err));
    }

#if CONFIG_AUDIO_STREAM_ENABLE
    err = audio_stream_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "audio_stream init failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "audio_stream ready");
    }
#endif

#if CONFIG_AUDIO_WS_ENABLE
    err = audio_ws_client_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "audio_ws_client init failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "audio_ws_client ready");
    }
#endif

#if CONFIG_ESP_SR_WAKE_ENABLE
    esp_sr_wake_set_callback(on_wakeword, NULL);
    err = esp_sr_wake_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp-sr wake start failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "esp-sr wake listening (say wake word)");
    }
#endif
}
#endif

#if CONFIG_ENV_SOUND_ENABLE
static void boot_env_sound(void)
{
    ESP_LOGI(TAG, "boot: env sound level");
    esp_err_t err = env_sound_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "env_sound start failed: %s", esp_err_to_name(err));
    }
}
#endif

void app_main(void)
{
#if CONFIG_YAMNET_VERIFY_ONLY
    ESP_LOGI(TAG, "YAMNet verify-only mode (no WiFi / sensors / Web)");

    esp_err_t err = ws2812_led_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WS2812 init failed: %s", esp_err_to_name(err));
    } else {
        (void)rgb_ui_init();
        rgb_ui_set_net(RGB_NET_SELFTEST);
    }

    err = ns4168_amp_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NS4168 init failed: %s", esp_err_to_name(err));
    }

#if CONFIG_YAMNET_ENABLE
    boot_audio();
    ESP_LOGI(TAG, "audio pipeline running — watch serial for YAMNet / wake logs");
#else
    ESP_LOGE(TAG, "Enable CONFIG_YAMNET_ENABLE in menuconfig");
#endif

#if CONFIG_ENV_SOUND_ENABLE
    boot_env_sound();
#endif

#else /* full application */

    web_log_init();
    ESP_LOGI(TAG, "MCP Sensors starting...");

    esp_err_t err = boot_platform();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(err));
        return;
    }

    boot_ui();
    
    boot_sensors();
    capture_session_init();

    err = boot_storage();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "web fs init failed: %s", esp_err_to_name(err));
        return;
    }

    err = boot_network();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi init failed: %s", esp_err_to_name(err));
        return;
    }
    (void)identify_btn_start();

#if CONFIG_YAMNET_ENABLE
    boot_audio();
#endif
#if CONFIG_ENV_SOUND_ENABLE
    boot_env_sound();
#endif

#endif /* YAMNET_VERIFY_ONLY */
}
