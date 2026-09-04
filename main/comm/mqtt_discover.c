/**
 * @file mqtt_discover.c
 * @brief Broker 配置存储。不查询 mDNS、不发现设备。
 */
#include "mqtt_discover.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "mqtt_cfg";
#define MQTT_NVS "mqtt_cfg"
static char s_uri[128];
static char s_user[64];
static char s_pass[64];
static TaskHandle_t s_task;
static volatile bool s_stop;

const char *mqtt_discover_get_broker_uri(void)
{
    return s_uri;
}

const char *mqtt_discover_get_username(void)
{
    return s_user[0] ? s_user : CONFIG_MQTT_USERNAME;
}

const char *mqtt_discover_get_password(void)
{
    return s_pass[0] ? s_pass : CONFIG_MQTT_PASSWORD;
}

static bool uri_is_loopback(const char *uri)
{
    const char *p = uri;
    if (!p || !p[0]) {
        return true;
    }
    if (strncmp(p, "mqtt://", 7) == 0) {
        p += 7;
    } else if (strncmp(p, "mqtts://", 8) == 0) {
        p += 8;
    }
    return strncmp(p, "127.", 4) == 0 || strncmp(p, "localhost", 9) == 0 ||
           strncmp(p, "[::1]", 5) == 0;
}

static void set_cred(char *dst, size_t dst_len, const char *src)
{
    if (!dst || dst_len == 0) {
        return;
    }
    dst[0] = '\0';
    if (!src || !src[0]) {
        return;
    }
    strncpy(dst, src, dst_len - 1);
    dst[dst_len - 1] = '\0';
}

static bool set_uri(const char *uri)
{
    if (!uri || !uri[0]) {
        return false;
    }
    if (strncmp(uri, "mqtt://", 7) != 0 && strncmp(uri, "mqtts://", 8) != 0) {
        ESP_LOGW(TAG, "ignore non-mqtt URI: %s", uri);
        return false;
    }
    if (uri_is_loopback(uri)) {
        ESP_LOGW(TAG, "ignore loopback broker: %s", uri);
        return false;
    }
    if (strcmp(s_uri, uri) == 0) {
        return true;
    }
    strncpy(s_uri, uri, sizeof(s_uri) - 1);
    s_uri[sizeof(s_uri) - 1] = '\0';
    ESP_LOGI(TAG, "MQTT broker: %s", s_uri);
    return true;
}

static void nvs_load(void)
{
    nvs_handle_t h;
    if (nvs_open(MQTT_NVS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t n = sizeof(s_uri);
    (void)nvs_get_str(h, "uri", s_uri, &n);
    n = sizeof(s_user);
    (void)nvs_get_str(h, "user", s_user, &n);
    n = sizeof(s_pass);
    (void)nvs_get_str(h, "pass", s_pass, &n);
    nvs_close(h);
    if (s_uri[0] && uri_is_loopback(s_uri)) {
        ESP_LOGW(TAG, "drop loopback broker in NVS: %s", s_uri);
        s_uri[0] = '\0';
        s_user[0] = '\0';
        s_pass[0] = '\0';
        return;
    }
    if (s_uri[0]) {
        ESP_LOGI(TAG, "MQTT broker from NVS: %s", s_uri);
    }
}

static void nvs_save(void)
{
    nvs_handle_t h;
    if (nvs_open(MQTT_NVS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    (void)nvs_set_str(h, "uri", s_uri);
    (void)nvs_set_str(h, "user", s_user);
    (void)nvs_set_str(h, "pass", s_pass);
    (void)nvs_commit(h);
    nvs_close(h);
}

esp_err_t mqtt_discover_apply(const char *uri, const char *user, const char *pass)
{
    if (!set_uri(uri)) {
        return ESP_ERR_INVALID_ARG;
    }
    set_cred(s_user, sizeof(s_user), user);
    set_cred(s_pass, sizeof(s_pass), pass);
    nvs_save();
    return ESP_OK;
}

esp_err_t mqtt_discover_refresh(void)
{
    if (s_uri[0]) {
        return ESP_OK;
    }
    nvs_load();
    return s_uri[0] ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static void task(void *arg)
{
    (void)arg;
    while (!s_stop) {
        (void)mqtt_discover_refresh();
        for (int i = 0; i < 50 && !s_stop; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t mqtt_discover_start(void)
{
    s_stop = false;
    if (s_task) {
        return ESP_OK;
    }
    nvs_load();
    (void)mqtt_discover_refresh();
    if (xTaskCreate(task, "mqtt_cfg", 3072, NULL, 3, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void mqtt_discover_stop(void)
{
    s_stop = true;
}
