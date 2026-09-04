#include "wifi_manager.h"

#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_hosted.h"
#include "captive_portal.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "wifi_mgr";

#define NVS_NAMESPACE   "wifi_cfg"
#define NVS_KEY_SSID    "ssid"
#define NVS_KEY_PASS    "pass"
#define NVS_KEY_DONE    "done"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define MAX_RETRY          20
#define FAST_FAIL_RETRY    3
#define HOSTED_RETRY_MAX   20
#define HOSTED_RETRY_MS    500
#define BOOT_CONNECT_DELAY_MS 4000
#define RETRY_DELAY_MS     2000

static EventGroupHandle_t s_wifi_events;
static wifi_on_connected_cb_t s_on_connected;
static wifi_on_ap_started_cb_t s_on_ap_started;
static wifi_on_connecting_cb_t s_on_connecting;
static wifi_on_connect_failed_cb_t s_on_failed;
static int s_retry_count;
static bool s_identify_ap;
static int s_retry_max;
static bool s_wifi_inited;
static bool s_hosted_inited;
static volatile bool s_switching_to_ap;
static volatile bool s_switching_to_sta;
static volatile bool s_connecting;
static volatile bool s_sta_want_connect; /**< 仅在主动连路由器时为 true，避免 APSTA 扫网触发乱连 */

static void ap_fallback_task(void *arg)
{
    (void)arg;
    /* 留出橙色闪烁时间，再切回 AP */
    vTaskDelay(pdMS_TO_TICKS(1600));
    wifi_manager_start_ap();
    vTaskDelete(NULL);
}

esp_err_t nvs_storage_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase (saved WiFi will be cleared): %s", esp_err_to_name(ret));
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "NVS initialized");
    } else {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

static esp_err_t ensure_event_loop(void)
{
    esp_err_t err = esp_event_loop_create_default();
    if (err == ESP_ERR_INVALID_STATE) {
        return ESP_OK;
    }
    return err;
}

static esp_err_t ensure_hosted_ready(void)
{
    esp_err_t err;

    if (!s_hosted_inited) {
        err = esp_hosted_init();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_hosted_init failed: %s", esp_err_to_name(err));
            return err;
        }
        s_hosted_inited = true;
    }

    for (int i = 0; i < HOSTED_RETRY_MAX; i++) {
        err = esp_hosted_connect_to_slave();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "ESP-Hosted transport ready (attempt %d)", i + 1);
            return ESP_OK;
        }
        ESP_LOGW(TAG, "hosted connect retry %d/%d: %s", i + 1, HOSTED_RETRY_MAX, esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(HOSTED_RETRY_MS));
    }

    ESP_LOGE(TAG, "ESP-Hosted transport not ready after %d retries", HOSTED_RETRY_MAX);
    return ESP_FAIL;
}

static esp_err_t nvs_load_credentials(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t done = 0;
    err = nvs_get_u8(handle, NVS_KEY_DONE, &done);
    if (err != ESP_OK || done != 1) {
        nvs_close(handle);
        return ESP_ERR_NOT_FOUND;
    }

    size_t len = ssid_len;
    err = nvs_get_str(handle, NVS_KEY_SSID, ssid, &len);
    if (err != ESP_OK) {
        nvs_close(handle);
        return err;
    }

    len = pass_len;
    err = nvs_get_str(handle, NVS_KEY_PASS, pass, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        pass[0] = '\0';
        err = ESP_OK;
    }
    nvs_close(handle);
    return err;
}

static esp_err_t nvs_save_credentials(const char *ssid, const char *password)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(handle, NVS_KEY_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, NVS_KEY_PASS, password);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, NVS_KEY_DONE, 1);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "WiFi credentials saved to NVS: ssid=%s", ssid);
    } else {
        ESP_LOGE(TAG, "failed to save WiFi credentials: %s", esp_err_to_name(err));
    }
    return err;
}

static void notify_connected(const char *ip_addr)
{
    if (s_on_connected) {
        s_on_connected(ip_addr);
    }
}

static const char *disconnect_reason_str(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_NO_AP_FOUND:
        return "找不到路由器(SSID错误或仅5GHz)";
    case WIFI_REASON_AUTH_FAIL:
        return "密码错误";
    case WIFI_REASON_ASSOC_FAIL:
        return "关联失败";
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        return "握手超时(密码错误或加密方式不兼容)";
    case WIFI_REASON_802_1X_AUTH_FAILED:
        return "802.1X认证失败(企业WiFi不支持)";
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        return "安全模式不兼容";
    default:
        return "未知原因";
    }
}

static bool is_ssid_not_found(uint8_t reason)
{
    return reason == WIFI_REASON_NO_AP_FOUND
        || reason == WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY
        || reason == WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD;
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        /* APSTA 配网/扫网也会拉起 STA，绝不能无脑 connect */
        if (s_sta_want_connect) {
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_switching_to_ap || s_switching_to_sta || !s_sta_want_connect) {
            return;
        }

        wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)data;
        ESP_LOGW(TAG, "STA disconnected, reason=%u (%s)", event->reason,
                 disconnect_reason_str(event->reason));

        if (is_ssid_not_found(event->reason)) {
            s_retry_max = FAST_FAIL_RETRY;
        } else if (s_retry_count == 0) {
            s_retry_max = MAX_RETRY;
        }

        if (s_retry_count < s_retry_max) {
            vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
            esp_wifi_connect();
            s_retry_count++;
            ESP_LOGW(TAG, "retry connect (%d/%d)", s_retry_count, s_retry_max);
        } else {
            s_connecting = false;
            s_sta_want_connect = false;
            xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
            ESP_LOGE(TAG, "connect failed after %d retries, fallback to AP (NVS credentials kept)",
                     s_retry_max);
            if (s_on_failed) {
                s_on_failed();
            }
            if (xTaskCreate(ap_fallback_task, "ap_fallback", 4096, NULL, 5, NULL) != pdPASS) {
                ESP_LOGE(TAG, "failed to schedule AP fallback");
            }
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        char ip_str[16];
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "got IP: %s", ip_str);
        s_retry_count = 0;
        s_connecting = false;
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
        notify_connected(ip_str);
    }
}

bool wifi_manager_is_configured(void)
{
    char ssid[33] = {0};
    char pass[65] = {0};
    return nvs_load_credentials(ssid, sizeof(ssid), pass, sizeof(pass)) == ESP_OK;
}

esp_err_t wifi_manager_clear_config(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_all(handle);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t wifi_manager_get_status(wifi_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(status, 0, sizeof(*status));
    status->connecting = s_connecting;

    char pass[65] = {0};
    if (nvs_load_credentials(status->ssid, sizeof(status->ssid), pass, sizeof(pass)) == ESP_OK) {
        status->configured = true;
        strncpy(status->password, pass, sizeof(status->password) - 1);
    }

    wifi_mode_t mode = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&mode) == ESP_OK) {
        /* 配网用 APSTA，未连上路由器时仍视为 AP 配网态 */
        status->ap_mode = (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA);
    }

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif != NULL) {
        esp_netif_ip_info_t ip_info = {0};
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
            status->connected = true;
            status->connecting = false;
            status->ap_mode = false;
            snprintf(status->ip, sizeof(status->ip), IPSTR, IP2STR(&ip_info.ip));
        }
    }

    return ESP_OK;
}

static const char *auth_mode_str(wifi_auth_mode_t mode)
{
    switch (mode) {
    case WIFI_AUTH_OPEN:
        return "OPEN";
    case WIFI_AUTH_WEP:
        return "WEP";
    case WIFI_AUTH_WPA_PSK:
        return "WPA";
    case WIFI_AUTH_WPA2_PSK:
        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:
        return "WPA2";
    case WIFI_AUTH_WPA3_PSK:
        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:
        return "WPA3";
    default:
        return "SEC";
    }
}

esp_err_t wifi_manager_scan(wifi_scan_ap_t *out, uint16_t max_count, uint16_t *out_count)
{
    if (out == NULL || max_count == 0 || out_count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_count = 0;

    wifi_mode_t mode = WIFI_MODE_NULL;
    (void)esp_wifi_get_mode(&mode);

    /*
     * 配网热点必须保持 AP 不掉线。纯 AP 无法扫网；切模式会踢掉手机客户端导致
     * HTTP 扫网请求收不到响应。因此：
     * - 已在 APSTA/STA：直接扫
     * - 仅 AP：升为 APSTA 并保持（不再切回 AP-only）
     */
    if (mode == WIFI_MODE_AP) {
        ESP_LOGI(TAG, "scan: promote AP -> APSTA (keep SoftAP clients)");
        esp_err_t mode_err = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (mode_err != ESP_OK) {
            ESP_LOGW(TAG, "set APSTA failed: %s", esp_err_to_name(mode_err));
            return mode_err;
        }
        /* 等 STA 接口起来；不 connect（s_sta_want_connect=false） */
        vTaskDelay(pdMS_TO_TICKS(200));
        mode = WIFI_MODE_APSTA;
    }

    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 120,
        .scan_time.active.max = 300,
    };

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi scan start failed: %s", esp_err_to_name(err));
        return err;
    }

    uint16_t ap_num = 0;
    err = esp_wifi_scan_get_ap_num(&ap_num);
    if (err != ESP_OK) {
        return err;
    }
    ESP_LOGI(TAG, "wifi scan done: %u APs", (unsigned)ap_num);
    if (ap_num == 0) {
        return ESP_OK;
    }

    wifi_ap_record_t *records = calloc(ap_num, sizeof(wifi_ap_record_t));
    if (records == NULL) {
        return ESP_ERR_NO_MEM;
    }

    uint16_t got = ap_num;
    err = esp_wifi_scan_get_ap_records(&got, records);
    if (err == ESP_OK) {
        uint16_t n = 0;
        for (uint16_t i = 0; i < got && n < max_count; i++) {
            if (records[i].ssid[0] == '\0') {
                continue;
            }
            /* 去重：同 SSID 保留信号更强的 */
            bool dup = false;
            for (uint16_t j = 0; j < n; j++) {
                if (strncmp(out[j].ssid, (const char *)records[i].ssid, sizeof(out[j].ssid)) == 0) {
                    if (records[i].rssi > out[j].rssi) {
                        out[j].rssi = records[i].rssi;
                        out[j].secure = (records[i].authmode != WIFI_AUTH_OPEN);
                        strncpy(out[j].auth, auth_mode_str(records[i].authmode), sizeof(out[j].auth) - 1);
                    }
                    dup = true;
                    break;
                }
            }
            if (dup) {
                continue;
            }
            strncpy(out[n].ssid, (const char *)records[i].ssid, sizeof(out[n].ssid) - 1);
            out[n].rssi = records[i].rssi;
            out[n].secure = (records[i].authmode != WIFI_AUTH_OPEN);
            strncpy(out[n].auth, auth_mode_str(records[i].authmode), sizeof(out[n].auth) - 1);
            n++;
        }
        *out_count = n;
        ESP_LOGI(TAG, "wifi scan unique SSIDs: %u", (unsigned)n);
    }
    free(records);
    return err;
}

esp_err_t wifi_manager_disconnect(void)
{
    ESP_LOGI(TAG, "disconnect: clear credentials and return to AP");
    s_connecting = false;
    s_sta_want_connect = false;
    (void)wifi_manager_clear_config();
    return wifi_manager_start_ap();
}

static void configure_ap_captive_portal(void)
{
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_netif == NULL) {
        ESP_LOGW(TAG, "AP netif not found");
        return;
    }

    esp_netif_dns_info_t dns = {
        .ip = {.type = ESP_IPADDR_TYPE_V4, .u_addr.ip4.addr = ESP_IP4TOADDR(192, 168, 4, 1)},
    };

    esp_netif_dhcps_stop(ap_netif);
    esp_netif_set_dns_info(ap_netif, ESP_NETIF_DNS_MAIN, &dns);

    static char captive_uri[] = "http://192.168.4.1";
    esp_err_t err = esp_netif_dhcps_option(ap_netif,
                                           ESP_NETIF_OP_SET,
                                           ESP_NETIF_CAPTIVEPORTAL_URI,
                                           captive_uri,
                                           strlen(captive_uri));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "DHCP captive portal URI not supported: %s", esp_err_to_name(err));
    }

    esp_netif_dhcps_start(ap_netif);
    ESP_LOGI(TAG, "AP DHCP DNS -> 192.168.4.1");
}

esp_err_t wifi_manager_enter_reprovision(void)
{
    ESP_LOGI(TAG, "enter reprovision mode (AP), saved credentials kept in NVS");
    s_retry_count = s_retry_max;
    return wifi_manager_start_ap();
}

esp_err_t wifi_manager_start_ap(void)
{
    s_switching_to_ap = true;
    s_sta_want_connect = false;
    s_connecting = false;
    captive_portal_start();

    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(200));

    wifi_config_t cfg = {
        .ap = {
            .ssid = "MCP-Sensors-Setup",
            .ssid_len = 0,
            .channel = 1,
            .password = "",
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN,
        },
    };

    /* APSTA：SoftAP 配网同时保留 STA 射频，扫网不必再切模式踢客户端 */
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set APSTA mode failed: %s", esp_err_to_name(err));
        s_switching_to_ap = false;
        return err;
    }
    err = esp_wifi_set_config(WIFI_IF_AP, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set AP config failed: %s", esp_err_to_name(err));
        s_switching_to_ap = false;
        return err;
    }
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start AP failed: %s", esp_err_to_name(err));
        s_switching_to_ap = false;
        return err;
    }
    configure_ap_captive_portal();

    ESP_LOGI(TAG, "APSTA started: SSID=%s, open http://192.168.4.1 (scan-ready)", cfg.ap.ssid);
    if (s_on_ap_started) {
        s_on_ap_started();
    }
    s_switching_to_ap = false;
    return ESP_OK;
}

esp_err_t wifi_manager_connect_sta(const char *ssid, const char *password)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    captive_portal_stop();

    ESP_ERROR_CHECK(nvs_save_credentials(ssid, password ? password : ""));

    wifi_config_t cfg = {0};
    strncpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid) - 1);
    if (password) {
        strncpy((char *)cfg.sta.password, password, sizeof(cfg.sta.password) - 1);
    }
    cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;

    s_switching_to_sta = true;
    s_connecting = true;
    s_sta_want_connect = true;
    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    s_retry_count = 0;
    s_retry_max = MAX_RETRY;
    xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    s_switching_to_sta = false;

    if (s_on_connecting) {
        s_on_connecting();
    }

    esp_wifi_connect();
    ESP_LOGI(TAG, "connecting to SSID: %s", ssid);
    return ESP_OK;
}

void wifi_manager_set_progress_cbs(wifi_on_connecting_cb_t on_connecting,
                                   wifi_on_connect_failed_cb_t on_failed)
{
    s_on_connecting = on_connecting;
    s_on_failed = on_failed;
}

esp_err_t wifi_manager_init(wifi_on_connected_cb_t on_connected, wifi_on_ap_started_cb_t on_ap_started)
{
    s_on_connected = on_connected;
    s_on_ap_started = on_ap_started;
    s_wifi_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(ensure_event_loop());
    ESP_ERROR_CHECK(ensure_hosted_ready());

    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    if (!s_wifi_inited) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        esp_err_t ret = esp_wifi_init(&cfg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(ret));
            return ret;
        }
        s_wifi_inited = true;
    }

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));

    char ssid[33] = {0};
    char pass[65] = {0};
    if (nvs_load_credentials(ssid, sizeof(ssid), pass, sizeof(pass)) == ESP_OK) {
        ESP_LOGI(TAG, "restored WiFi from NVS: ssid=%s, wait %dms for coprocessor...",
                 ssid, BOOT_CONNECT_DELAY_MS);
        vTaskDelay(pdMS_TO_TICKS(BOOT_CONNECT_DELAY_MS));
        return wifi_manager_connect_sta(ssid, pass);
    }

    ESP_LOGI(TAG, "no WiFi in NVS, starting AP provisioning");
    return wifi_manager_start_ap();
}

esp_err_t wifi_manager_start_identify_ap(void)
{
    if (s_identify_ap) {
        return ESP_OK;
    }
    wifi_config_t cfg = {
        .ap = {
            .ssid = "MCP-Sensors-Find",
            .channel = 1,
            .password = "",
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN,
        },
    };
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_wifi_set_config(WIFI_IF_AP, &cfg);
    if (err != ESP_OK) {
        return err;
    }
    configure_ap_captive_portal();
    captive_portal_set_redirect("http://192.168.4.1/identify");
    captive_portal_start();
    s_identify_ap = true;
    ESP_LOGI(TAG, "identify APSTA SSID=%s -> http://192.168.4.1/identify", cfg.ap.ssid);
    return ESP_OK;
}

void wifi_manager_stop_identify_ap(void)
{
    if (!s_identify_ap) {
        return;
    }
    captive_portal_set_redirect("http://192.168.4.1/");
    captive_portal_stop();
    (void)esp_wifi_set_mode(WIFI_MODE_STA);
    s_identify_ap = false;
    ESP_LOGI(TAG, "identify AP stopped");
}

bool wifi_manager_is_identify_ap(void)
{
    return s_identify_ap;
}
