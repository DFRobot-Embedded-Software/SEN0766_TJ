/**
 * @file mcp_gw_client.c
 * @brief mDNS 发现聚合网关 + POST register/heartbeat
 */
#include "mcp_gw_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mdns.h"
#include "sdkconfig.h"

#include "mcp_core.h"

static const char *TAG = "mcp_gw";

esp_err_t mcp_gw_device_id(char *out, size_t out_len)
{
    if (!out || out_len < 7) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t mac[6] = {0};
    esp_err_t err = ESP_FAIL;

    /* P4 + ESP-Hosted：STA MAC 在网卡/netif 上，esp_read_mac(WIFI_STA) 常失败 */
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        err = esp_netif_get_mac(netif, mac);
    }
    if (err != ESP_OK) {
        err = esp_read_mac(mac, ESP_MAC_EFUSE_FACTORY);
    }
    if (err != ESP_OK) {
        err = esp_read_mac(mac, ESP_MAC_BASE);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "read MAC failed: %s", esp_err_to_name(err));
        return err;
    }
    snprintf(out, out_len, "%02x%02x%02x", mac[3], mac[4], mac[5]);
    return ESP_OK;
}

#if CONFIG_MCP_GW_CLIENT_ENABLE

static TaskHandle_t s_task;
static char s_local_ip[16];
static char s_device_id[8];
static char s_register_url[256];
static char s_voice_ws_uri[128];
static volatile bool s_stop;

const char *mcp_gw_get_voice_ws_uri(void)
{
    return s_voice_ws_uri;
}

static void set_voice_ws_from_host(const char *ip)
{
    if (!ip || !ip[0]) {
        return;
    }
    snprintf(s_voice_ws_uri, sizeof(s_voice_ws_uri), "ws://%s:8765/ws", ip);
    ESP_LOGI(TAG, "voice WS URI: %s", s_voice_ws_uri);
}

static void use_fallback_register_url(void)
{
    strncpy(s_register_url, CONFIG_MCP_GW_REGISTER_URL, sizeof(s_register_url) - 1);
    s_register_url[sizeof(s_register_url) - 1] = '\0';
    ESP_LOGI(TAG, "register URL fallback: %s", s_register_url);

    /* 从回退 register URL 推断主机，拼语音 WS */
    const char *p = strstr(s_register_url, "://");
    if (p) {
        p += 3;
        char host[64] = {0};
        size_t i = 0;
        while (p[i] && p[i] != ':' && p[i] != '/' && i + 1 < sizeof(host)) {
            host[i] = p[i];
            i++;
        }
        if (host[0]) {
            set_voice_ws_from_host(host);
        }
    }
#if CONFIG_AUDIO_WS_ENABLE
    if (!s_voice_ws_uri[0] && CONFIG_AUDIO_WS_URI[0] != '\0') {
        strncpy(s_voice_ws_uri, CONFIG_AUDIO_WS_URI, sizeof(s_voice_ws_uri) - 1);
        s_voice_ws_uri[sizeof(s_voice_ws_uri) - 1] = '\0';
        ESP_LOGI(TAG, "voice WS URI from Kconfig: %s", s_voice_ws_uri);
    }
#endif
}

#if CONFIG_MCP_GW_MDNS_DISCOVER
static const char *txt_get(const mdns_result_t *r, const char *key)
{
    if (!r || !r->txt) {
        return NULL;
    }
    for (size_t i = 0; i < r->txt_count; ++i) {
        if (r->txt[i].key && strcmp(r->txt[i].key, key) == 0) {
            return r->txt[i].value;
        }
    }
    return NULL;
}

static bool discover_register_url(void)
{
    mdns_result_t *results = NULL;
    esp_err_t err = mdns_query_ptr("_mcp-gw", "_tcp", 3000, 8, &results);
    if (err != ESP_OK || !results) {
        ESP_LOGW(TAG, "mDNS browse _mcp-gw failed: %s", esp_err_to_name(err));
        return false;
    }

    bool ok = false;
    for (mdns_result_t *r = results; r; r = r->next) {
        const char *path = txt_get(r, "path");
        if (!path || !path[0]) {
            path = "/api/devices/register";
        }
        char path_buf[96];
        if (path[0] == '/') {
            strncpy(path_buf, path, sizeof(path_buf) - 1);
            path_buf[sizeof(path_buf) - 1] = '\0';
        } else {
            snprintf(path_buf, sizeof(path_buf), "/%s", path);
        }

        uint16_t port = r->port ? r->port : 9080;
        char ip[16] = {0};
        for (mdns_ip_addr_t *a = r->addr; a; a = a->next) {
            if (a->addr.type == ESP_IPADDR_TYPE_V4) {
                esp_ip4addr_ntoa(&a->addr.u_addr.ip4, ip, sizeof(ip));
                break;
            }
        }
        if (!ip[0] && r->hostname) {
            esp_ip4_addr_t addr4 = {0};
            if (mdns_query_a(r->hostname, 2000, &addr4) == ESP_OK) {
                esp_ip4addr_ntoa(&addr4, ip, sizeof(ip));
            }
        }
        if (!ip[0]) {
            continue;
        }

        snprintf(s_register_url, sizeof(s_register_url), "http://%s:%u%s", ip, (unsigned)port, path_buf);
        ESP_LOGI(TAG, "mDNS found gateway instance=%s url=%s",
                 r->instance_name ? r->instance_name : "?", s_register_url);

        const char *ws = txt_get(r, "ws");
        if (ws && ws[0] && (strncmp(ws, "ws://", 5) == 0 || strncmp(ws, "wss://", 6) == 0)) {
            strncpy(s_voice_ws_uri, ws, sizeof(s_voice_ws_uri) - 1);
            s_voice_ws_uri[sizeof(s_voice_ws_uri) - 1] = '\0';
            ESP_LOGI(TAG, "mDNS voice WS: %s", s_voice_ws_uri);
        } else {
            set_voice_ws_from_host(ip);
        }
        ok = true;
        break;
    }
    mdns_query_results_free(results);
    return ok;
}
#endif /* CONFIG_MCP_GW_MDNS_DISCOVER */

static void resolve_register_url(void)
{
#if CONFIG_MCP_GW_MDNS_DISCOVER
    if (discover_register_url()) {
        return;
    }
    ESP_LOGW(TAG, "mDNS discover miss, use Kconfig URL");
#endif
    use_fallback_register_url();
}

static esp_err_t http_post_json(const char *url, const char *json_body, int *out_status)
{
    if (out_status) {
        *out_status = 0;
    }

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 8000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    if (CONFIG_MCP_GW_TOKEN[0] != '\0') {
        esp_http_client_set_header(client, "X-MCP-GW-Token", CONFIG_MCP_GW_TOKEN);
    }

    esp_err_t err = esp_http_client_open(client, strlen(json_body));
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }
    int wlen = esp_http_client_write(client, json_body, strlen(json_body));
    if (wlen < 0) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }
    (void)esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (out_status) {
        *out_status = status;
    }

    char buf[256];
    int rlen = esp_http_client_read(client, buf, sizeof(buf) - 1);
    if (rlen > 0) {
        buf[rlen] = '\0';
        ESP_LOGI(TAG, "HTTP %d: %s", status, buf);
    } else {
        ESP_LOGI(TAG, "HTTP %d", status);
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return (status >= 200 && status < 300) ? ESP_OK : ESP_FAIL;
}

static char *build_payload(bool heartbeat)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    cJSON_AddStringToObject(root, "id", s_device_id);
    cJSON_AddStringToObject(root, "ip", s_local_ip);
    cJSON_AddNumberToObject(root, "port", MCP_HTTP_PORT);
    cJSON_AddStringToObject(root, "path", MCP_ENDPOINT_URI);
    if (CONFIG_MCP_GW_DEVICE_NAME[0] != '\0') {
        cJSON_AddStringToObject(root, "name", CONFIG_MCP_GW_DEVICE_NAME);
    }
    if (!heartbeat) {
        cJSON_AddStringToObject(root, "fw", "mcp_sensors");
    }
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return s;
}

static esp_err_t do_register(void)
{
    if (!s_register_url[0]) {
        resolve_register_url();
    }
    char *body = build_payload(false);
    if (!body) {
        return ESP_ERR_NO_MEM;
    }
    int status = 0;
    esp_err_t err = http_post_json(s_register_url, body, &status);
    free(body);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "registered id=%s -> %s", s_device_id, s_register_url);
    } else {
        ESP_LOGW(TAG, "register failed id=%s status=%d url=%s", s_device_id, status, s_register_url);
        /* 下次重试时允许重新 mDNS */
        s_register_url[0] = '\0';
    }
    return err;
}

static esp_err_t do_heartbeat(void)
{
    char url[288];
    strncpy(url, s_register_url, sizeof(url) - 1);
    url[sizeof(url) - 1] = '\0';
    char *p = strstr(url, "/register");
    if (p) {
        *p = '\0';
        char tmp[320];
        snprintf(tmp, sizeof(tmp), "%s/%s/heartbeat", url, s_device_id);
        strncpy(url, tmp, sizeof(url) - 1);
        url[sizeof(url) - 1] = '\0';
    } else {
        snprintf(url, sizeof(url), "%s", s_register_url);
    }

    char *body = build_payload(true);
    if (!body) {
        return ESP_ERR_NO_MEM;
    }
    int status = 0;
    esp_err_t err = http_post_json(url, body, &status);
    free(body);
    return err;
}

static void gw_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "task start id=%s ip=%s", s_device_id, s_local_ip);
#if CONFIG_MCP_GW_MDNS_DISCOVER
    ESP_LOGI(TAG, "will try mDNS browse _mcp-gw._tcp then fallback URL");
#else
    ESP_LOGI(TAG, "mDNS discover disabled, use Kconfig URL");
#endif
    /* MCP httpd + 本机 mDNS 刚起来，稍等再 browse/注册 */
    vTaskDelay(pdMS_TO_TICKS(1500));
    resolve_register_url();
    ESP_LOGI(TAG, "using register URL: %s", s_register_url);

    int backoff_ms = 2000;
    while (!s_stop) {
        if (do_register() == ESP_OK) {
            break;
        }
        if (s_stop) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(backoff_ms));
        if (backoff_ms < 60000) {
            backoff_ms *= 2;
        }
    }

    const int hb_ms = CONFIG_MCP_GW_HEARTBEAT_S * 1000;
    while (!s_stop) {
        for (int i = 0; i < hb_ms / 500 && !s_stop; ++i) {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        if (s_stop) {
            break;
        }
        if (do_heartbeat() != ESP_OK) {
            ESP_LOGW(TAG, "heartbeat failed, try re-register");
            (void)do_register();
        }
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t mcp_gw_client_start(const char *local_ip)
{
    if (!local_ip || !local_ip[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    mcp_gw_client_stop();

    esp_err_t err = mcp_gw_device_id(s_device_id, sizeof(s_device_id));
    if (err != ESP_OK) {
        return err;
    }
    strncpy(s_local_ip, local_ip, sizeof(s_local_ip) - 1);
    s_local_ip[sizeof(s_local_ip) - 1] = '\0';
    s_register_url[0] = '\0';
    s_voice_ws_uri[0] = '\0';
    s_stop = false;

    ESP_LOGI(TAG, "starting client device_id=%s local_ip=%s", s_device_id, s_local_ip);
    BaseType_t ok = xTaskCreate(gw_task, "mcp_gw", 7168, NULL, 5, &s_task);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void mcp_gw_client_stop(void)
{
    s_stop = true;
    if (s_task) {
        for (int i = 0; i < 30 && s_task; ++i) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (s_task) {
            vTaskDelete(s_task);
            s_task = NULL;
        }
    }
}

#else /* !CONFIG_MCP_GW_CLIENT_ENABLE */

const char *mcp_gw_get_voice_ws_uri(void)
{
    return "";
}

esp_err_t mcp_gw_client_start(const char *local_ip)
{
    (void)local_ip;
    return ESP_ERR_NOT_SUPPORTED;
}

void mcp_gw_client_stop(void) {}

#endif
