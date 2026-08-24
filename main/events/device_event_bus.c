/**
 * @file device_event_bus.c
 * @brief 统一设备事件环形日志与订阅
 */

#include "device_event_bus.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "radar_area.h"
#include "sdkconfig.h"

static const char *TAG = "event_bus";

#ifndef CONFIG_DEVICE_EVENT_LOG_SIZE
#ifdef CONFIG_MCP_EVENT_LOG_SIZE
#define CONFIG_DEVICE_EVENT_LOG_SIZE CONFIG_MCP_EVENT_LOG_SIZE
#else
#define CONFIG_DEVICE_EVENT_LOG_SIZE 32
#endif
#endif

#define EVENT_JSON_MAX 1024

typedef struct {
    bool valid;
    uint8_t area_id;
    char category[16];
    char type[32];
    char json[EVENT_JSON_MAX];
} event_record_t;

typedef struct {
    device_event_notify_fn fn;
    void *ctx;
} subscriber_t;

static event_record_t s_log[CONFIG_DEVICE_EVENT_LOG_SIZE];
static uint8_t s_next;
static uint8_t s_count;
static SemaphoreHandle_t s_lock;
static subscriber_t s_subs[4];
static uint8_t s_sub_count;
static bool s_inited;

static void format_timestamp(char *buf, size_t len)
{
    time_t now = time(NULL);
    struct tm tm_local;

    if (now > 1700000000 && localtime_r(&now, &tm_local) != NULL) {
        strftime(buf, len, "%Y-%m-%d %H:%M", &tm_local);
        return;
    }

    uint64_t uptime_s = (uint64_t)(xTaskGetTickCount() * portTICK_PERIOD_MS / 1000U);
    snprintf(buf, len, "uptime+%" PRIu64 "s", uptime_s);
}

esp_err_t device_event_bus_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memset(s_log, 0, sizeof(s_log));
    memset(s_subs, 0, sizeof(s_subs));
    s_next = 0;
    s_count = 0;
    s_sub_count = 0;
    s_inited = true;
    ESP_LOGI(TAG, "device event bus ready (log=%u)", CONFIG_DEVICE_EVENT_LOG_SIZE);
    return ESP_OK;
}

esp_err_t device_event_subscribe(device_event_notify_fn fn, void *ctx)
{
    if (!s_inited || fn == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_sub_count >= (uint8_t)(sizeof(s_subs) / sizeof(s_subs[0]))) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NO_MEM;
    }
    s_subs[s_sub_count].fn = fn;
    s_subs[s_sub_count].ctx = ctx;
    s_sub_count++;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

void device_event_clear(void)
{
    if (!s_inited) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(s_log, 0, sizeof(s_log));
    s_next = 0;
    s_count = 0;
    xSemaphoreGive(s_lock);
}

esp_err_t device_event_emit(const device_event_t *ev)
{
    if (!s_inited || ev == NULL || ev->type == NULL || ev->type[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    const char *category = (ev->category && ev->category[0]) ? ev->category : DEVICE_EVENT_CAT_MMWAVE;

    char ts[32];
    format_timestamp(ts, sizeof(ts));

    cJSON *json = cJSON_CreateObject();
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(json, "type", ev->type);
    cJSON_AddStringToObject(json, "category", category);
    cJSON_AddStringToObject(json, "timestamp", ts);
    if (ev->area_id != DEVICE_EVENT_AREA_ID_GLOBAL) {
        cJSON_AddNumberToObject(json, "area_id", ev->area_id);
    } else {
        cJSON_AddNumberToObject(json, "area_id", DEVICE_EVENT_AREA_ID_GLOBAL);
    }
    if (ev->area != NULL && ev->area[0] != '\0') {
        cJSON_AddStringToObject(json, "area", ev->area);
    }
    if (ev->person_id >= 0) {
        cJSON_AddNumberToObject(json, "person_id", ev->person_id);
    }
    if (ev->active >= 0) {
        cJSON_AddNumberToObject(json, "active", ev->active);
    }
    if (ev->static_count >= 0) {
        cJSON_AddNumberToObject(json, "static", ev->static_count);
    }
    if (ev->message != NULL) {
        cJSON_AddStringToObject(json, "message", ev->message);
    }
    if (ev->payload != NULL && cJSON_IsObject((const cJSON *)ev->payload)) {
        cJSON *dup = cJSON_Duplicate((const cJSON *)ev->payload, true);
        if (dup != NULL) {
            cJSON_AddItemToObject(json, "payload", dup);
        }
    }

    char *payload = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (payload == NULL) {
        return ESP_ERR_NO_MEM;
    }

    size_t payload_len = strlen(payload);
    if (payload_len >= EVENT_JSON_MAX) {
        ESP_LOGW(TAG, "event json too long (%u >= %u), drop type=%s",
                 (unsigned)payload_len, (unsigned)EVENT_JSON_MAX, ev->type);
        free(payload);
        return ESP_ERR_INVALID_SIZE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    event_record_t *slot = &s_log[s_next];
    slot->valid = true;
    slot->area_id = ev->area_id;
    strncpy(slot->category, category, sizeof(slot->category) - 1);
    slot->category[sizeof(slot->category) - 1] = '\0';
    strncpy(slot->type, ev->type, sizeof(slot->type) - 1);
    slot->type[sizeof(slot->type) - 1] = '\0';
    memcpy(slot->json, payload, payload_len + 1U);
    s_next = (uint8_t)((s_next + 1U) % CONFIG_DEVICE_EVENT_LOG_SIZE);
    if (s_count < CONFIG_DEVICE_EVENT_LOG_SIZE) {
        s_count++;
    }

    subscriber_t subs_copy[4];
    uint8_t sub_n = s_sub_count;
    memcpy(subs_copy, s_subs, sizeof(subs_copy));
    xSemaphoreGive(s_lock);

    free(payload);

    for (uint8_t i = 0; i < sub_n; i++) {
        if (subs_copy[i].fn != NULL) {
            subs_copy[i].fn(ev->type, ev->area_id, subs_copy[i].ctx);
        }
    }

    ESP_LOGI(TAG, "emit %s/%s area=%u", category, ev->type, (unsigned)ev->area_id);
    return ESP_OK;
}

static bool event_visible_in_area_feed(const event_record_t *rec, uint8_t area_id)
{
    if (rec->area_id == area_id) {
        return true;
    }
    if (rec->area_id != DEVICE_EVENT_AREA_ID_GLOBAL) {
        return false;
    }
    return strcmp(rec->type, DEVICE_EVENT_ROOM_EMPTY) == 0 ||
           strcmp(rec->type, DEVICE_EVENT_RANGE_ENTER) == 0 ||
           strcmp(rec->type, DEVICE_EVENT_RANGE_EXIT) == 0;
}

static void append_areas_array(cJSON *root)
{
    area_config_t cfg = {0};
    if (area_config_get(&cfg) != ESP_OK) {
        return;
    }
    cJSON *areas = cJSON_AddArrayToObject(root, "areas");
    if (areas == NULL) {
        return;
    }
    for (uint8_t i = 0; i < cfg.tag_count; i++) {
        const area_tag_t *tag = &cfg.tags[i];
        cJSON *item = cJSON_CreateObject();
        if (item == NULL) {
            continue;
        }
        cJSON_AddNumberToObject(item, "id", i);
        cJSON_AddStringToObject(item, "name", tag->name);
        cJSON_AddNumberToObject(item, "strategy", tag->strategy);
        cJSON_AddItemToArray(areas, item);
    }
}

/* 调用方须已持 s_lock；只做字符串拷贝，不做 cJSON_Parse */
static uint8_t copy_filtered_event_jsons(uint8_t filter_area_id,
                                         bool filter_area,
                                         const char *category_filter,
                                         char *out_copies[],
                                         uint8_t out_cap)
{
    uint8_t copy_n = 0;
    if (out_copies == NULL || out_cap == 0 || s_count == 0) {
        return 0;
    }

    uint8_t start = (uint8_t)((s_next + CONFIG_DEVICE_EVENT_LOG_SIZE - s_count) %
                              CONFIG_DEVICE_EVENT_LOG_SIZE);
    for (uint8_t n = 0; n < s_count; n++) {
        uint8_t idx = (uint8_t)((start + n) % CONFIG_DEVICE_EVENT_LOG_SIZE);
        const event_record_t *rec = &s_log[idx];
        if (!rec->valid) {
            continue;
        }
        if (filter_area && !event_visible_in_area_feed(rec, filter_area_id)) {
            continue;
        }
        if (category_filter != NULL && category_filter[0] != '\0' &&
            strcmp(rec->category, category_filter) != 0) {
            continue;
        }
        if (copy_n >= out_cap) {
            break;
        }
        out_copies[copy_n] = strdup(rec->json);
        if (out_copies[copy_n] != NULL) {
            copy_n++;
        }
    }
    return copy_n;
}

static void parse_copies_into_array(cJSON *events, char *copies[], uint8_t copy_n)
{
    if (events == NULL || copies == NULL) {
        return;
    }
    for (uint8_t i = 0; i < copy_n; i++) {
        if (copies[i] == NULL) {
            continue;
        }
        cJSON *item = cJSON_Parse(copies[i]);
        free(copies[i]);
        copies[i] = NULL;
        if (item != NULL) {
            cJSON_AddItemToArray(events, item);
        }
    }
}

static void add_events_summary(cJSON *root, cJSON *events)
{
    const int n = cJSON_GetArraySize(events);
    if (n <= 0) {
        return;
    }

    size_t cap = 64;
    char *summary = (char *)malloc(cap);
    if (summary == NULL) {
        return;
    }
    summary[0] = '\0';

    /* 摘要只取最近几条，避免超长字符串拖慢 MCP 响应 */
    const int start = (n > 8) ? (n - 8) : 0;
    for (int i = start; i < n; i++) {
        const cJSON *item = cJSON_GetArrayItem(events, i);
        const cJSON *msg = cJSON_GetObjectItem(item, "message");
        if (!cJSON_IsString(msg)) {
            continue;
        }
        const char *text = msg->valuestring;
        const size_t need = strlen(summary) + strlen(text) + 4;
        if (need >= cap) {
            cap = need + 32;
            char *grown = (char *)realloc(summary, cap);
            if (grown == NULL) {
                break;
            }
            summary = grown;
        }
        if (summary[0] != '\0') {
            strcat(summary, " | ");
        }
        strcat(summary, text);
    }

    if (summary[0] != '\0') {
        cJSON_AddStringToObject(root, "summary", summary);
    }
    free(summary);

    if (n > 0) {
        cJSON *last = cJSON_GetArrayItem(events, n - 1);
        if (last != NULL) {
            cJSON_AddItemToObject(root, "latest", cJSON_Duplicate(last, true));
        }
    }
}

static char *build_feed_json(uint8_t filter_area_id,
                             bool filter_area,
                             const char *category_filter)
{
    area_config_t cfg = {0};
    (void)area_config_get(&cfg);

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }

    if (filter_area) {
        if (filter_area_id >= cfg.tag_count) {
            cJSON_Delete(root);
            return NULL;
        }
        cJSON_AddNumberToObject(root, "area_id", filter_area_id);
        cJSON_AddStringToObject(root, "name", cfg.tags[filter_area_id].name);
        cJSON_AddNumberToObject(root, "strategy", cfg.tags[filter_area_id].strategy);
    } else {
        append_areas_array(root);
    }

    if (category_filter != NULL && category_filter[0] != '\0') {
        cJSON_AddStringToObject(root, "category", category_filter);
    }

    cJSON *events = cJSON_AddArrayToObject(root, "events");
    if (events == NULL) {
        cJSON_Delete(root);
        return NULL;
    }

    /* 锁内只拷贝字符串；解锁后再 Parse，避免与 emit 互相拖死 */
    char *copies[CONFIG_DEVICE_EVENT_LOG_SIZE];
    memset(copies, 0, sizeof(copies));
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint8_t copy_n = copy_filtered_event_jsons(filter_area_id, filter_area, category_filter,
                                               copies, CONFIG_DEVICE_EVENT_LOG_SIZE);
    xSemaphoreGive(s_lock);
    parse_copies_into_array(events, copies, copy_n);

    add_events_summary(root, events);
    cJSON_AddBoolToObject(root, "ready", cJSON_GetArraySize(events) > 0);

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return payload;
}

esp_err_t device_event_read_feed(char **out_json)
{
    return device_event_read_feed_filtered(NULL, out_json);
}

esp_err_t device_event_read_feed_filtered(const char *category_filter, char **out_json)
{
    if (!s_inited || out_json == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    *out_json = build_feed_json(0, false, category_filter);
    return (*out_json != NULL) ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t device_event_read_area(uint8_t area_id, char **out_json)
{
    if (!s_inited || out_json == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    area_config_t cfg = {0};
    if (area_config_get(&cfg) != ESP_OK || area_id >= cfg.tag_count) {
        return ESP_ERR_NOT_FOUND;
    }
    *out_json = build_feed_json(area_id, true, NULL);
    return (*out_json != NULL) ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t device_event_read_latest_by_category(const char *category, char **out_json)
{
    if (!s_inited || out_json == NULL || category == NULL || category[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    *out_json = NULL;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_count == 0) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }
    uint8_t start = (uint8_t)((s_next + CONFIG_DEVICE_EVENT_LOG_SIZE - s_count) %
                              CONFIG_DEVICE_EVENT_LOG_SIZE);
    const event_record_t *found = NULL;
    for (uint8_t n = s_count; n > 0; n--) {
        uint8_t idx = (uint8_t)((start + n - 1U) % CONFIG_DEVICE_EVENT_LOG_SIZE);
        const event_record_t *rec = &s_log[idx];
        if (!rec->valid) {
            continue;
        }
        if (strcmp(rec->category, category) == 0) {
            found = rec;
            break;
        }
    }
    if (found == NULL) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }

    char *copy = strdup(found->json);
    xSemaphoreGive(s_lock);
    if (copy == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON *src = cJSON_Parse(copy);
    free(copy);
    if (src == NULL) {
        return ESP_ERR_NO_MEM;
    }
    *out_json = cJSON_PrintUnformatted(src);
    cJSON_Delete(src);
    return (*out_json != NULL) ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t device_event_fill_recent(const char *category, cJSON *out_array, int max_count)
{
    if (!s_inited || out_array == NULL || !cJSON_IsArray(out_array)) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_count == 0) {
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }

    uint8_t start = (uint8_t)((s_next + CONFIG_DEVICE_EVENT_LOG_SIZE - s_count) %
                              CONFIG_DEVICE_EVENT_LOG_SIZE);
    /* 从新到旧收集，锁内只拷贝字符串 */
    char *copies[CONFIG_DEVICE_EVENT_LOG_SIZE];
    uint8_t copy_n = 0;
    memset(copies, 0, sizeof(copies));
    for (uint8_t n = s_count; n > 0; n--) {
        uint8_t idx = (uint8_t)((start + n - 1U) % CONFIG_DEVICE_EVENT_LOG_SIZE);
        const event_record_t *rec = &s_log[idx];
        if (!rec->valid) {
            continue;
        }
        if (category != NULL && category[0] != '\0' &&
            strcmp(rec->category, category) != 0) {
            continue;
        }
        copies[copy_n] = strdup(rec->json);
        if (copies[copy_n] != NULL) {
            copy_n++;
        }
        if (max_count > 0 && copy_n >= (uint8_t)max_count) {
            break;
        }
    }
    xSemaphoreGive(s_lock);

    /* 新→旧收集，反转为旧→新追加 */
    for (uint8_t i = copy_n; i > 0; i--) {
        char *raw = copies[i - 1U];
        copies[i - 1U] = NULL;
        if (raw == NULL) {
            continue;
        }
        cJSON *item = cJSON_Parse(raw);
        free(raw);
        if (item != NULL) {
            cJSON_AddItemToArray(out_array, item);
        }
    }
    return ESP_OK;
}

esp_err_t device_event_read_latest_mmwave(char **out_json)
{
    if (!s_inited || out_json == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    *out_json = NULL;

    char *raw = NULL;
    esp_err_t err = device_event_read_latest_by_category(DEVICE_EVENT_CAT_MMWAVE, &raw);
    if (err != ESP_OK || raw == NULL) {
        return err;
    }

    cJSON *src = cJSON_Parse(raw);
    free(raw);
    if (src == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON *out = cJSON_CreateObject();
    if (out == NULL) {
        cJSON_Delete(src);
        return ESP_ERR_NO_MEM;
    }
    const cJSON *area = cJSON_GetObjectItem(src, "area");
    const cJSON *type = cJSON_GetObjectItem(src, "type");
    const cJSON *msg = cJSON_GetObjectItem(src, "message");
    const cJSON *ts = cJSON_GetObjectItem(src, "timestamp");
    const cJSON *area_id = cJSON_GetObjectItem(src, "area_id");
    const cJSON *person_id = cJSON_GetObjectItem(src, "person_id");
    /* 与 recent_events 条目字段对齐，避免 UI/MCP 读到另一套键名 */
    cJSON_AddStringToObject(out, "type", cJSON_IsString(type) ? type->valuestring : "");
    cJSON_AddStringToObject(out, "category", DEVICE_EVENT_CAT_MMWAVE);
    cJSON_AddStringToObject(out, "area", cJSON_IsString(area) ? area->valuestring : "毫米波");
    cJSON_AddStringToObject(out, "message",
                            cJSON_IsString(msg) ? msg->valuestring :
                            (cJSON_IsString(type) ? type->valuestring : "—"));
    cJSON_AddStringToObject(out, "timestamp", cJSON_IsString(ts) ? ts->valuestring : "—");
    if (cJSON_IsNumber(area_id)) {
        cJSON_AddNumberToObject(out, "area_id", area_id->valuedouble);
    }
    if (cJSON_IsNumber(person_id)) {
        cJSON_AddNumberToObject(out, "person_id", person_id->valuedouble);
    }
    /* 兼容旧 UI：保留 name/event/time 别名 */
    cJSON_AddStringToObject(out, "name", cJSON_IsString(area) ? area->valuestring : "毫米波");
    cJSON_AddStringToObject(out, "event",
                            cJSON_IsString(msg) ? msg->valuestring :
                            (cJSON_IsString(type) ? type->valuestring : "—"));
    cJSON_AddStringToObject(out, "time", cJSON_IsString(ts) ? ts->valuestring : "—");
    cJSON_Delete(src);

    *out_json = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    return (*out_json != NULL) ? ESP_OK : ESP_ERR_NO_MEM;
}

bool device_event_area_has_unpaired_enter(uint8_t area_id)
{
    if (!s_inited) {
        return false;
    }
    bool open = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_count == 0) {
        xSemaphoreGive(s_lock);
        return false;
    }
    uint8_t start = (uint8_t)((s_next + CONFIG_DEVICE_EVENT_LOG_SIZE - s_count) %
                              CONFIG_DEVICE_EVENT_LOG_SIZE);
    for (uint8_t n = s_count; n > 0; n--) {
        uint8_t idx = (uint8_t)((start + n - 1U) % CONFIG_DEVICE_EVENT_LOG_SIZE);
        const event_record_t *rec = &s_log[idx];
        if (!rec->valid || rec->area_id != area_id) {
            continue;
        }
        if (strcmp(rec->type, DEVICE_EVENT_AREA_EXIT) == 0) {
            break;
        }
        if (strcmp(rec->type, DEVICE_EVENT_AREA_ENTER) == 0) {
            open = true;
            break;
        }
    }
    xSemaphoreGive(s_lock);
    return open;
}
