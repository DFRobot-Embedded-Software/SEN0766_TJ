/**
 * @file voice_discover.c
 * @brief 浏览网关 mDNS：_voice-ws 拿 PCM；_mcp-gw TXT mqtt= 写入 mqtt_discover。
 */
#include "voice_discover.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "mdns.h"
#include "mqtt_discover.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "voice_mdns";
static char s_uri[128];
static TaskHandle_t s_task;
static volatile bool s_stop;

const char *voice_discover_get_ws_uri(void)
{
    return s_uri;
}

static void set_uri(const char *uri)
{
    if (!uri || !uri[0]) {
        return;
    }
    strncpy(s_uri, uri, sizeof(s_uri) - 1);
    s_uri[sizeof(s_uri) - 1] = '\0';
    ESP_LOGI(TAG, "voice WS URI: %s", s_uri);
}

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

static bool ipv4_of(const mdns_result_t *r, char *ip, size_t ip_len)
{
    if (!r || !ip || ip_len < 8) {
        return false;
    }
    ip[0] = '\0';
    for (mdns_ip_addr_t *a = r->addr; a; a = a->next) {
        if (a->addr.type == ESP_IPADDR_TYPE_V4) {
            esp_ip4addr_ntoa(&a->addr.u_addr.ip4, ip, ip_len);
            return ip[0] != '\0';
        }
    }
    return false;
}

static bool browse_voice_ws(void)
{
    mdns_result_t *results = NULL;
    if (mdns_query_ptr("_voice-ws", "_tcp", 2500, 8, &results) != ESP_OK || !results) {
        return false;
    }
    bool ok = false;
    for (mdns_result_t *r = results; r; r = r->next) {
        const char *ws = txt_get(r, "ws");
        if (ws && strncmp(ws, "ws://", 5) == 0) {
            set_uri(ws);
            ok = true;
            break;
        }
        char ip[16] = {0};
        if (!ipv4_of(r, ip, sizeof(ip))) {
            continue;
        }
        uint16_t port = r->port ? r->port : 8765;
        char uri[128];
        snprintf(uri, sizeof(uri), "ws://%s:%u/ws", ip, (unsigned)port);
        set_uri(uri);
        ok = true;
        break;
    }
    mdns_query_results_free(results);
    return ok;
}

static bool browse_mcp_gw(void)
{
    mdns_result_t *results = NULL;
    if (mdns_query_ptr("_mcp-gw", "_tcp", 2500, 8, &results) != ESP_OK || !results) {
        return false;
    }
    bool mqtt_ok = false;
    for (mdns_result_t *r = results; r; r = r->next) {
        const char *mqtt = txt_get(r, "mqtt");
        if (mqtt && mqtt[0] &&
            mqtt_discover_apply(mqtt, txt_get(r, "mqtt_user"), txt_get(r, "mqtt_pass")) == ESP_OK) {
            mqtt_ok = true;
        }
        if (!s_uri[0]) {
            const char *ws = txt_get(r, "ws");
            if (ws && strncmp(ws, "ws://", 5) == 0) {
                set_uri(ws);
            }
        }
        if (mqtt_ok) {
            break;
        }
    }
    mdns_query_results_free(results);
    return mqtt_ok;
}

static void task(void *arg)
{
    (void)arg;
    while (!s_stop) {
        if (!s_uri[0]) {
            (void)browse_voice_ws();
#if CONFIG_AUDIO_WS_ENABLE
            if (!s_uri[0] && CONFIG_AUDIO_WS_URI[0] != '\0') {
                set_uri(CONFIG_AUDIO_WS_URI);
            }
#endif
        }
        /* 访客 Wi‑Fi 常拦 mDNS：WS 与 mqtt TXT 分开刷，不能因 WS 成功就跳过 mcp-gw */
        if (!mqtt_discover_get_broker_uri()[0]) {
            (void)browse_mcp_gw();
        } else if (!s_uri[0]) {
            (void)browse_mcp_gw();
        }
        for (int i = 0; i < 50 && !s_stop; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t voice_discover_start(void)
{
    s_stop = false;
    s_uri[0] = '\0';
    if (s_task) {
        return ESP_OK;
    }
    if (xTaskCreate(task, "voice_mdns", 4096, NULL, 3, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void voice_discover_stop(void)
{
    s_stop = true;
}
