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
#include "mdns.h"
#include "mcp_server.h"
#include "mcp_gw_client.h"
#include "board_snapshot.h"
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

static void on_wifi_connected(const char *ip_addr)
{
    static char s_mdns_id[8];
    char host[32] = "mcp-sensors";
    char inst[40] = "MCP Sensors";
    s_mdns_id[0] = '\0';
    if (mcp_gw_device_id(s_mdns_id, sizeof(s_mdns_id)) == ESP_OK) {
        snprintf(host, sizeof(host), "mcp-sensors-%s", s_mdns_id);
        snprintf(inst, sizeof(inst), "MCP Sensors %s", s_mdns_id);
    }

    ESP_LOGI(TAG, "boot: phase6 online");
    ESP_LOGI(TAG, "WiFi connected:");
    ESP_LOGI(TAG, "  Web UI: http://%s or http://%s.local", ip_addr, host);
    ESP_LOGI(TAG, "  MCP:    http://%s:%d%s", ip_addr, MCP_HTTP_PORT, MCP_ENDPOINT_URI);
    ESP_LOGI(TAG, "  device_id: %s", s_mdns_id[0] ? s_mdns_id : "(unknown)");

    rgb_ui_set_net(RGB_NET_OK);

    mdns_init();
    mdns_hostname_set(host);
    mdns_instance_name_set(inst);
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    /* 便于同网发现；注册仍以 mcp_gw HTTP 为准 */
    mdns_txt_item_t mcp_txt[] = {
        {.key = "path", .value = MCP_ENDPOINT_URI},
        {.key = "id", .value = s_mdns_id},
    };
    mdns_service_add(NULL, "_mcp", "_tcp", MCP_HTTP_PORT, mcp_txt, 2);

    init_time_sync();

    web_server_stop();
    mcp_server_stop();
#if CONFIG_MCP_GW_CLIENT_ENABLE
    mcp_gw_client_stop();
#endif

    esp_err_t err = mcp_server_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MCP server start failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "MCP server up, starting mcp_gw client (if enabled)");

#if CONFIG_MCP_GW_CLIENT_ENABLE
    /* 不依赖 Web 仪表盘；MCP 起来即可向聚合网关注册 */
    err = mcp_gw_client_start(ip_addr);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mcp_gw client start failed: %s", esp_err_to_name(err));
    }
#endif

    err = web_server_start_dashboard();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "web server start failed: %s", esp_err_to_name(err));
        /* MCP / mcp_gw 已启动，不因网页失败而停掉 */
    } else {
        err = web_server_register_static_routes();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "static routes register failed: %s", esp_err_to_name(err));
        }
    }

    /* 联网 + 核心服务均已起来，确认本次（可能是 OTA 后的）固件可用，取消回滚保护 */
    ota_update_mark_valid();
}

static void on_ap_started(void)
{
    /* 失败闪灯会自行切回 AP_WAIT；其它进入 AP 的路径直接显示等待配网 */
    if (rgb_ui_get_net() != RGB_NET_FAIL) {
        rgb_ui_set_net(RGB_NET_AP_WAIT);
    }
#if CONFIG_MCP_GW_CLIENT_ENABLE
    mcp_gw_client_stop();
#endif
    mcp_server_stop();
    mdns_free();
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
    ESP_LOGI(TAG, "YAMNet verify-only mode (no WiFi / sensors / MCP / Web)");

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

#if CONFIG_YAMNET_ENABLE
    boot_audio();
#endif
#if CONFIG_ENV_SOUND_ENABLE
    boot_env_sound();
#endif

#endif /* YAMNET_VERIFY_ONLY */
}
