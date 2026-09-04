/**
 * @file mqtt_hub.c
 * @brief 实现 mqtt_hub.h 的线上契约。改 topic/载荷/retain 必须同步改：
 *        本文件、mqtt_hub.h、device_event_bus、mcp_gw/mqtt_events.py、HA const.py。
 *
 * Broker 只来自 _mcp-gw TXT mqtt=（voice_discover → mqtt_discover_apply）。
 * MQTT 不浏览设备、不浏览 _mqtt._tcp。
 * 未连上时事件入队，避免网页有事件、MQTT 一条都没有。
 *
 * 鉴权失败时禁止反复 destroy 客户端（esp-mqtt outbox 易堆损坏）；
 * 仅在 URI/账号变更时安全拆除重建。
 */

#include "mqtt_hub.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "device_id.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "mqtt_discover.h"
#include "sdkconfig.h"

static const char *TAG = "mqtt_hub";

#define MQTT_PENDING_MAX 32

static esp_mqtt_client_handle_t s_client;
static char s_id[8];
static char s_event_topic[64];
static char s_avail_topic[64];
static char s_using_uri[128];
static char s_open_user[64];
static char s_open_pass[64];
static bool s_ready;
static volatile bool s_stop;
static volatile bool s_auth_denied;
static TaskHandle_t s_wait_task;
static SemaphoreHandle_t s_q_lock;
static char *s_pending[MQTT_PENDING_MAX];
static uint8_t s_pending_n;
static int s_unready_loops;

static void ensure_lock(void)
{
    if (!s_q_lock) {
        s_q_lock = xSemaphoreCreateMutex();
    }
}

bool mqtt_hub_is_connected(void)
{
    return s_ready && s_client != NULL;
}

const char *mqtt_hub_broker_uri(void)
{
    const char *d = mqtt_discover_get_broker_uri();
    if (s_using_uri[0]) {
        return s_using_uri;
    }
    return d ? d : "";
}

const char *mqtt_hub_event_topic(void)
{
    return s_event_topic[0] ? s_event_topic : "";
}

static void pending_push(const char *json)
{
    ensure_lock();
    char *copy = strdup(json);
    if (!copy) {
        return;
    }
    xSemaphoreTake(s_q_lock, portMAX_DELAY);
    if (s_pending_n >= MQTT_PENDING_MAX) {
        free(s_pending[0]);
        memmove(&s_pending[0], &s_pending[1], (MQTT_PENDING_MAX - 1) * sizeof(s_pending[0]));
        s_pending_n = MQTT_PENDING_MAX - 1;
    }
    s_pending[s_pending_n++] = copy;
    uint8_t n = s_pending_n;
    xSemaphoreGive(s_q_lock);
    ESP_LOGW(TAG, "queued event (mqtt not ready, pending=%u)", (unsigned)n);
}

static void pending_flush(void)
{
    if (!s_ready || !s_client || s_event_topic[0] == '\0') {
        return;
    }
    ensure_lock();
    for (;;) {
        char *json = NULL;
        xSemaphoreTake(s_q_lock, portMAX_DELAY);
        if (s_pending_n == 0) {
            xSemaphoreGive(s_q_lock);
            break;
        }
        json = s_pending[0];
        memmove(&s_pending[0], &s_pending[1], (s_pending_n - 1) * sizeof(s_pending[0]));
        s_pending_n--;
        xSemaphoreGive(s_q_lock);
        if (json) {
            int mid = esp_mqtt_client_publish(s_client, s_event_topic, json, 0, 1, 0);
            ESP_LOGI(TAG, "flush event mid=%d topic=%s", mid, s_event_topic);
            free(json);
        }
    }
}

static void on_mqtt(void *args, esp_event_base_t base, int32_t id, void *data)
{
    (void)args;
    (void)base;
    if (id == MQTT_EVENT_CONNECTED) {
        s_ready = true;
        s_auth_denied = false;
        s_unready_loops = 0;
        if (s_client) {
            esp_mqtt_client_publish(s_client, s_avail_topic, "online", 0, 1, 1);
        }
        ESP_LOGI(TAG, "connected %s -> %s", s_using_uri, s_event_topic);
        pending_flush();
    } else if (id == MQTT_EVENT_DISCONNECTED) {
        s_ready = false;
        ESP_LOGW(TAG, "disconnected");
    } else if (id == MQTT_EVENT_ERROR) {
        const esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)data;
        if (ev && ev->error_handle &&
            ev->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
            const int code = (int)ev->error_handle->connect_return_code;
            ESP_LOGW(TAG, "connection refused code=%d (broker rejected login)", code);
            if (code == MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED ||
                code == MQTT_CONNECTION_REFUSE_BAD_USERNAME) {
                s_auth_denied = true;
                ESP_LOGW(TAG,
                         "MQTT not authorized: clear device mqtt user/pass or match broker; "
                         "will not destroy client in a loop");
            }
        } else {
            ESP_LOGW(TAG, "client error");
        }
    }
}

/** 安全拆除：先摘句柄，再 disconnect → stop → destroy，避免 outbox 双释放。 */
static void mqtt_client_teardown(void)
{
    s_ready = false;
    esp_mqtt_client_handle_t c = s_client;
    s_client = NULL;
    if (c == NULL) {
        s_using_uri[0] = '\0';
        s_open_user[0] = '\0';
        s_open_pass[0] = '\0';
        return;
    }

    (void)esp_mqtt_client_disconnect(c);
    vTaskDelay(pdMS_TO_TICKS(150));
    (void)esp_mqtt_client_stop(c);
    vTaskDelay(pdMS_TO_TICKS(200));
    (void)esp_mqtt_client_destroy(c);
    vTaskDelay(pdMS_TO_TICKS(50));

    s_using_uri[0] = '\0';
    s_open_user[0] = '\0';
    s_open_pass[0] = '\0';
}

static esp_err_t mqtt_client_open(const char *uri)
{
    if (!uri || !uri[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *user = mqtt_discover_get_username();
    const char *pass = mqtt_discover_get_password();
    if (!user) {
        user = "";
    }
    if (!pass) {
        pass = "";
    }
    if (s_client && strcmp(s_using_uri, uri) == 0 && strcmp(s_open_user, user) == 0 &&
        strcmp(s_open_pass, pass) == 0) {
        return ESP_OK;
    }

    s_auth_denied = false;
    mqtt_client_teardown();

    if (device_id_get(s_id, sizeof(s_id)) != ESP_OK) {
        return ESP_FAIL;
    }
    snprintf(s_event_topic, sizeof(s_event_topic), "mcp_sensors/%s/event", s_id);
    snprintf(s_avail_topic, sizeof(s_avail_topic), "mcp_sensors/%s/availability", s_id);

    strncpy(s_using_uri, uri, sizeof(s_using_uri) - 1);
    s_using_uri[sizeof(s_using_uri) - 1] = '\0';
    strncpy(s_open_user, user, sizeof(s_open_user) - 1);
    s_open_user[sizeof(s_open_user) - 1] = '\0';
    strncpy(s_open_pass, pass, sizeof(s_open_pass) - 1);
    s_open_pass[sizeof(s_open_pass) - 1] = '\0';

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = s_using_uri,
        .credentials.client_id = s_id,
        .session.last_will.topic = s_avail_topic,
        .session.last_will.qos = 1,
        .session.last_will.retain = 1,
        .session.last_will.msg = "offline",
        .network.timeout_ms = 10000,
        .network.reconnect_timeout_ms = 10000,
    };
    if (user[0]) {
        cfg.credentials.username = user;
    }
    if (pass[0]) {
        cfg.credentials.authentication.password = pass;
    }

    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) {
        s_using_uri[0] = '\0';
        s_open_user[0] = '\0';
        s_open_pass[0] = '\0';
        return ESP_FAIL;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, on_mqtt, NULL);
    ESP_LOGI(TAG, "connecting %s topic=%s user=%s", s_using_uri, s_event_topic, user[0] ? user : "(none)");
    return esp_mqtt_client_start(s_client);
}

static void wait_task(void *arg)
{
    (void)arg;
    while (!s_stop) {
        if (!s_ready) {
            (void)mqtt_discover_refresh();
        }
        const char *uri = mqtt_discover_get_broker_uri();
        if (uri && uri[0]) {
            const char *user = mqtt_discover_get_username();
            const char *pass = mqtt_discover_get_password();
            if (!user) {
                user = "";
            }
            if (!pass) {
                pass = "";
            }
            const bool same_endpoint = s_client && strcmp(s_using_uri, uri) == 0 &&
                                       strcmp(s_open_user, user) == 0 &&
                                       strcmp(s_open_pass, pass) == 0;

            if (same_endpoint && !s_ready) {
                s_unready_loops++;
                /* 鉴权失败：只 soft disconnect，绝不 destroy 循环 */
                if (s_auth_denied) {
                    if (s_unready_loops >= 30) {
                        ESP_LOGW(TAG,
                                 "auth still denied after ~60s, soft disconnect (keep client); "
                                 "user=%s broker=%s",
                                 user[0] ? user : "(none)", uri);
                        (void)esp_mqtt_client_disconnect(s_client);
                        s_unready_loops = 0;
                    }
                } else if (s_unready_loops >= 15) {
                    ESP_LOGW(TAG, "still not connected after ~30s, soft disconnect %s", uri);
                    (void)esp_mqtt_client_disconnect(s_client);
                    s_unready_loops = 0;
                }
            } else if (!same_endpoint) {
                if (mqtt_client_open(uri) != ESP_OK) {
                    ESP_LOGW(TAG, "mqtt open failed: %s", uri);
                }
            }
        } else {
            ESP_LOGW(TAG, "no broker yet (wait _mcp-gw TXT mqtt= / HTTP action=mqtt)");
        }

        const int ticks = s_auth_denied ? 50 : 20;
        for (int i = 0; i < ticks && !s_stop; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    s_wait_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t mqtt_hub_start(void)
{
    ensure_lock();
#if CONFIG_COMM_ROLE_LEAF
    if (mqtt_discover_refresh() != ESP_OK) {
        ESP_LOGW(TAG, "no MQTT broker (wait _mcp-gw TXT mqtt=)");
        return ESP_OK;
    }
    return mqtt_client_open(mqtt_discover_get_broker_uri());
#else
    s_stop = false;
    esp_err_t err = mqtt_discover_start();
    if (err != ESP_OK) {
        return err;
    }
    if (!s_wait_task) {
        if (xTaskCreate(wait_task, "mqtt_wait", 4096, NULL, 4, &s_wait_task) != pdPASS) {
            return ESP_ERR_NO_MEM;
        }
    }
    const char *uri = mqtt_discover_get_broker_uri();
    if (uri && uri[0]) {
        return mqtt_client_open(uri);
    }
    return ESP_OK;
#endif
}

void mqtt_hub_stop(void)
{
    s_stop = true;
    mqtt_discover_stop();
    mqtt_client_teardown();
    for (int i = 0; i < 40 && s_wait_task; i++) {
        vTaskDelay(pdMS_TO_TICKS(25));
    }
}

void mqtt_hub_publish_event(const char *json)
{
    if (!json || !json[0]) {
        return;
    }
#if CONFIG_COMM_ROLE_LEAF
    if (mqtt_hub_start() != ESP_OK) {
        pending_push(json);
        return;
    }
    for (int i = 0; i < 40 && !s_ready; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!s_ready || !s_client) {
        pending_push(json);
        mqtt_hub_stop();
        return;
    }
    pending_flush();
    int mid = esp_mqtt_client_publish(s_client, s_event_topic, json, 0, 1, 0);
    ESP_LOGI(TAG, "publish mid=%d %s", mid, s_event_topic);
    vTaskDelay(pdMS_TO_TICKS(400));
    mqtt_hub_stop();
#else
    if (!s_ready || !s_client || s_event_topic[0] == '\0') {
        pending_push(json);
        return;
    }
    int mid = esp_mqtt_client_publish(s_client, s_event_topic, json, 0, 1, 0);
    if (mid < 0) {
        ESP_LOGW(TAG, "publish failed mid=%d, queue", mid);
        pending_push(json);
        return;
    }
    ESP_LOGI(TAG, "publish mid=%d %s", mid, s_event_topic);
#endif
}
