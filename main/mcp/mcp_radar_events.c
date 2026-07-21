/**
 * @file mcp_radar_events.c
 * @brief C4004 区域事件检测
 *
 * 事件写入环形缓冲区，read 时附带当前 area_config 中的区域列表。
 * 用户通过 Web 增删区域后无需改代码，resources/list 由 mcp_server 动态同步。
 */

#include "mcp_radar_events.h"

#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "c4004_sensor.h"
#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "sensor_data.h"

static const char *TAG = "mcp_events";

#define EVENT_MSG_LEN 160
#define EVENT_JSON_MAX 384
#ifndef CONFIG_MCP_EVENT_LOG_SIZE
#define CONFIG_MCP_EVENT_LOG_SIZE 32
#endif
#ifndef CONFIG_MCP_RADAR_OCC_DEBOUNCE_MS
#define CONFIG_MCP_RADAR_OCC_DEBOUNCE_MS 1500
#endif
#ifndef CONFIG_MCP_RADAR_CROSSING_COOLDOWN_MS
#define CONFIG_MCP_RADAR_CROSSING_COOLDOWN_MS 5000
#endif

typedef struct {
    bool valid;
    uint8_t area_id;
    char json[EVENT_JSON_MAX];
} event_record_t;

typedef struct {
    bool last_enter;
    bool last_exit;
    int reported_total;
    int pending_total;
    TickType_t count_stable_since;
    TickType_t occupied_since;
    bool dwell_fired;
    /** 已上报 enter 且尚未配对 exit（与 reported_total 互补，防止计数不同步） */
    bool session_open;
} tag_track_t;

static event_record_t s_event_log[CONFIG_MCP_EVENT_LOG_SIZE];
static uint8_t s_event_log_next;
static uint8_t s_event_log_count;

static SemaphoreHandle_t s_lock;
static QueueHandle_t s_notify_queue;
static TaskHandle_t s_watch_task;
static TaskHandle_t s_notify_task;
static mcp_radar_event_notify_cb s_notify_cb;
static bool s_running;
static bool s_room_was_occupied;
static TickType_t s_post_room_empty_guard_until;
static TickType_t s_last_room_empty_at;
static tag_track_t s_tag_track[AREA_TAG_MAX];
/** 上一 poll 已发 area_exit，下一 poll 再发 room_empty（避免客户端合并 read） */
static bool s_room_empty_pending;
static bool s_vacated_area_notify[AREA_TAG_MAX];

/** 仅 watch 任务使用，避免 area_config_t + sensor_snapshot_t (~2KB) 占任务栈 */
static area_config_t s_poll_cfg;
static sensor_snapshot_t s_poll_snap;

#define MCP_RADAR_WATCH_STACK  8192
#define MCP_RADAR_NOTIFY_STACK 4096

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

static void append_areas_array(cJSON *root, const area_config_t *cfg)
{
    cJSON *areas = cJSON_AddArrayToObject(root, "areas");
    if (areas == NULL || cfg == NULL) {
        return;
    }

    for (uint8_t i = 0; i < cfg->tag_count; i++) {
        const area_tag_t *tag = &cfg->tags[i];
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

static void get_tag_people_count(uint8_t idx,
                                 const area_config_t *cfg,
                                 const sensor_snapshot_t *snap,
                                 int *active,
                                 int *static_count)
{
    const area_tag_t *tag = &cfg->tags[idx];

    if (tag->strategy == 1) {
        *active = snap->areas[idx].active;
        *static_count = snap->areas[idx].static_count;
        return;
    }

    area_rect_t rect = {0};
    if (area_coords_to_rect_for_radar(tag, &rect) == ESP_OK) {
        if (c4004_sensor_count_targets_in_rect(&rect, active, static_count) != ESP_OK) {
            *active = 0;
            *static_count = 0;
        }
        return;
    }

    *active = 0;
    *static_count = 0;
}

static bool is_room_occupied(const area_config_t *cfg, const sensor_snapshot_t *snap)
{
    if (cfg->tag_count == 0) {
        return false;
    }

    for (uint8_t i = 0; i < cfg->tag_count; i++) {
        int active = 0;
        int static_count = 0;
        get_tag_people_count(i, cfg, snap, &active, &static_count);
        if (active + static_count > 0) {
            return true;
        }
    }
    return false;
}

static bool push_event(uint8_t area_id, cJSON *json, bool notify)
{
    char *payload = cJSON_PrintUnformatted(json);
    if (payload == NULL) {
        return false;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    event_record_t *slot = &s_event_log[s_event_log_next];
    slot->valid = true;
    slot->area_id = area_id;
    strncpy(slot->json, payload, sizeof(slot->json) - 1);
    slot->json[sizeof(slot->json) - 1] = '\0';

    s_event_log_next = (uint8_t)((s_event_log_next + 1U) % CONFIG_MCP_EVENT_LOG_SIZE);
    if (s_event_log_count < CONFIG_MCP_EVENT_LOG_SIZE) {
        s_event_log_count++;
    }
    xSemaphoreGive(s_lock);

    free(payload);

    if (notify && s_notify_queue != NULL) {
        (void)xQueueSend(s_notify_queue, &area_id, 0);
    }
    return true;
}

static bool emit_room_empty(bool notify)
{
    const TickType_t now = xTaskGetTickCount();
    const TickType_t cooldown = pdMS_TO_TICKS(CONFIG_MCP_RADAR_CROSSING_COOLDOWN_MS);
    if (s_last_room_empty_at != 0 && (now - s_last_room_empty_at) < cooldown) {
        return false;
    }

    char ts[32];
    format_timestamp(ts, sizeof(ts));

    cJSON *json = cJSON_CreateObject();
    if (json == NULL) {
        return false;
    }

    cJSON_AddStringToObject(json, "type", "room_empty");
    cJSON_AddStringToObject(json, "timestamp", ts);
    cJSON_AddStringToObject(json, "message", "房间无人");

    if (!push_event(MCP_EVENT_AREA_ID_GLOBAL, json, notify)) {
        cJSON_Delete(json);
        return false;
    }
    s_last_room_empty_at = now;
    cJSON_Delete(json);
    ESP_LOGI(TAG, "event room_empty @ %s", ts);
    return true;
}

static bool emit_area_crossing(uint8_t area_id,
                               bool is_enter,
                               const char *area_name,
                               int active,
                               int static_count,
                               bool notify)
{
    char ts[32];
    format_timestamp(ts, sizeof(ts));

    char message[EVENT_MSG_LEN];
    if (is_enter) {
        snprintf(message, sizeof(message),
                 "有人进入%s区域，该区域有%d人活动，%d人静止",
                 area_name, active, static_count);
    } else {
        snprintf(message, sizeof(message),
                 "有人离开%s区域，该区域有%d人活动，%d人静止",
                 area_name, active, static_count);
    }

    cJSON *json = cJSON_CreateObject();
    if (json == NULL) {
        return false;
    }

    cJSON_AddStringToObject(json, "type", is_enter ? "area_enter" : "area_exit");
    cJSON_AddNumberToObject(json, "area_id", area_id);
    cJSON_AddStringToObject(json, "area", area_name);
    cJSON_AddStringToObject(json, "timestamp", ts);
    cJSON_AddNumberToObject(json, "active", active);
    cJSON_AddNumberToObject(json, "static", static_count);
    cJSON_AddStringToObject(json, "message", message);

    if (!push_event(area_id, json, notify)) {
        cJSON_Delete(json);
        return false;
    }
    cJSON_Delete(json);

    if (is_enter) {
        s_post_room_empty_guard_until = 0;
        s_tag_track[area_id].session_open = true;
    }

    ESP_LOGI(TAG, "event %s @ %s area[%u]=%s active=%d static=%d",
             is_enter ? "area_enter" : "area_exit", ts, area_id, area_name, active, static_count);
    return true;
}

static void emit_area_dwell(uint8_t area_id,
                            const char *area_name,
                            int active,
                            int static_count)
{
    char ts[32];
    format_timestamp(ts, sizeof(ts));

    char message[EVENT_MSG_LEN];
    snprintf(message, sizeof(message),
             "有人在%s区域停留过久，该区域有%d人活动，%d人静止",
             area_name, active, static_count);

    cJSON *json = cJSON_CreateObject();
    if (json == NULL) {
        return;
    }

    cJSON_AddStringToObject(json, "type", "area_dwell");
    cJSON_AddNumberToObject(json, "area_id", area_id);
    cJSON_AddStringToObject(json, "area", area_name);
    cJSON_AddStringToObject(json, "timestamp", ts);
    cJSON_AddNumberToObject(json, "active", active);
    cJSON_AddNumberToObject(json, "static", static_count);
    cJSON_AddStringToObject(json, "message", message);

    push_event(area_id, json, true);
    cJSON_Delete(json);
    ESP_LOGI(TAG, "event area_dwell @ %s area[%u]=%s active=%d static=%d",
             ts, area_id, area_name, active, static_count);
}

static bool event_visible_in_area_feed(const event_record_t *rec, uint8_t area_id)
{
    if (rec->area_id == area_id) {
        return true;
    }
    if (rec->area_id != MCP_EVENT_AREA_ID_GLOBAL) {
        return false;
    }
    /* room_empty 同步出现在各区域 feed，便于只订阅 sensor://area/{id}/events 的客户端 */
    return strstr(rec->json, "\"type\":\"room_empty\"") != NULL;
}

static void append_events_to_array(cJSON *events, uint8_t filter_area_id, bool filter_enabled)
{
    if (events == NULL || s_event_log_count == 0) {
        return;
    }

    uint8_t start = (uint8_t)((s_event_log_next + CONFIG_MCP_EVENT_LOG_SIZE - s_event_log_count) %
                              CONFIG_MCP_EVENT_LOG_SIZE);

    for (uint8_t n = 0; n < s_event_log_count; n++) {
        uint8_t idx = (uint8_t)((start + n) % CONFIG_MCP_EVENT_LOG_SIZE);
        const event_record_t *rec = &s_event_log[idx];
        if (!rec->valid) {
            continue;
        }
        /* 区域 feed：本区域事件 + room_empty（不含其他全局事件） */
        if (filter_enabled && !event_visible_in_area_feed(rec, filter_area_id)) {
            continue;
        }

        cJSON *item = cJSON_Parse(rec->json);
        if (item != NULL) {
            cJSON_AddItemToArray(events, item);
        }
    }
}

static void add_events_summary(cJSON *root, cJSON *events)
{
    if (root == NULL || events == NULL) {
        return;
    }

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

    for (int i = 0; i < n; i++) {
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

    /* 便于只读单条的客户端：latest 始终指向 events 最后一条 */
    if (n > 0) {
        cJSON *last = cJSON_GetArrayItem(events, n - 1);
        if (last != NULL) {
            cJSON_AddItemToObject(root, "latest", cJSON_Duplicate(last, true));
        }
    }
}

static char *build_feed_json_locked(const area_config_t *cfg, uint8_t filter_area_id, bool filter_enabled)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }

    if (filter_enabled) {
        if (cfg == NULL || filter_area_id >= cfg->tag_count) {
            cJSON_Delete(root);
            return NULL;
        }
        cJSON_AddNumberToObject(root, "area_id", filter_area_id);
        cJSON_AddStringToObject(root, "name", cfg->tags[filter_area_id].name);
        cJSON_AddNumberToObject(root, "strategy", cfg->tags[filter_area_id].strategy);
    } else {
        cJSON_AddBoolToObject(root, "enabled", cfg != NULL && cfg->enabled);
        append_areas_array(root, cfg);
    }

    cJSON *events = cJSON_AddArrayToObject(root, "events");
    if (events == NULL) {
        cJSON_Delete(root);
        return NULL;
    }

    append_events_to_array(events, filter_area_id, filter_enabled);
    add_events_summary(root, events);
    cJSON_AddBoolToObject(root, "ready", cJSON_GetArraySize(events) > 0);

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return payload;
}

static bool should_suppress_area_exit(bool room_occupied, int stable_count, TickType_t now)
{
    /* 人数已归零（最后离开）时不抑制，避免只剩 room_empty 没有 area_exit */
    if (stable_count == 0) {
        return false;
    }
    return !room_occupied &&
           s_post_room_empty_guard_until != 0 &&
           now < s_post_room_empty_guard_until;
}

static void notify_immediate(uint8_t area_id)
{
    if (s_notify_cb != NULL) {
        s_notify_cb(area_id);
    }
}

/** 事件日志中该区域是否有未配对的 area_enter */
static bool area_log_has_unpaired_enter(uint8_t area_id)
{
    if (s_event_log_count == 0) {
        return false;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    const uint8_t start = (uint8_t)((s_event_log_next + CONFIG_MCP_EVENT_LOG_SIZE -
                                     s_event_log_count) % CONFIG_MCP_EVENT_LOG_SIZE);
    bool open = false;

    for (uint8_t n = s_event_log_count; n > 0; n--) {
        const uint8_t idx = (uint8_t)((start + n - 1U) % CONFIG_MCP_EVENT_LOG_SIZE);
        const event_record_t *rec = &s_event_log[idx];
        if (!rec->valid) {
            continue;
        }

        if (rec->area_id == area_id) {
            if (strstr(rec->json, "\"type\":\"area_exit\"") != NULL) {
                break;
            }
            if (strstr(rec->json, "\"type\":\"area_enter\"") != NULL) {
                open = true;
                break;
            }
        }
    }

    xSemaphoreGive(s_lock);
    return open;
}

static void clear_vacated_area_notify(void)
{
    memset(s_vacated_area_notify, 0, sizeof(s_vacated_area_notify));
}

static void schedule_room_empty(uint8_t area_id)
{
    if (area_id < AREA_TAG_MAX) {
        s_vacated_area_notify[area_id] = true;
    }
    s_room_empty_pending = true;
}

/** 延迟到下一 poll：房间仍空则发 room_empty 并各 URI 只通知一次 */
static void flush_pending_room_empty(const area_config_t *cfg, bool room_occupied)
{
    if (!s_room_empty_pending) {
        return;
    }

    if (room_occupied) {
        s_room_empty_pending = false;
        clear_vacated_area_notify();
        return;
    }

    if (!emit_room_empty(false)) {
        s_room_empty_pending = false;
        clear_vacated_area_notify();
        return;
    }

    notify_immediate(MCP_EVENT_AREA_ID_GLOBAL);
    for (uint8_t i = 0; i < cfg->tag_count && i < AREA_TAG_MAX; i++) {
        if (s_vacated_area_notify[i]) {
            notify_immediate(i);
        }
    }

    s_room_empty_pending = false;
    clear_vacated_area_notify();
    ESP_LOGI(TAG, "room_empty flushed (deferred notify)");
}

/**
 * 房间由有人→无人（边沿）：本 poll 只补 area_exit 并 notify；room_empty 下一 poll。
 */
static void handle_room_just_vacated(const area_config_t *cfg, const sensor_snapshot_t *snap)
{
    const TickType_t now = xTaskGetTickCount();
    bool any_exit = false;

    for (uint8_t i = 0; i < cfg->tag_count && i < AREA_TAG_MAX; i++) {
        tag_track_t *track = &s_tag_track[i];
        const int reported = track->reported_total > 0 ? track->reported_total : 0;
        const bool log_open = area_log_has_unpaired_enter(i);
        const bool need_exit = reported > 0 || track->session_open || log_open;
        if (!need_exit) {
            continue;
        }

        int active = 0;
        int static_count = 0;
        get_tag_people_count(i, cfg, snap, &active, &static_count);
        const area_tag_t *tag = &cfg->tags[i];
        const int to_emit = reported > 0 ? reported : 1;

        ESP_LOGI(TAG, "vacate area[%u] emit %d exit(s) (reported=%d session_open=%d log_open=%d)",
                 i, to_emit, reported, track->session_open, log_open);
        for (int n = 0; n < to_emit; n++) {
            (void)emit_area_crossing(i, false, tag->name, active, static_count, false);
        }

        track->reported_total = 0;
        track->pending_total = 0;
        track->count_stable_since = 0;
        track->session_open = false;
        schedule_room_empty(i);
        notify_immediate(i);
        any_exit = true;
    }

    if (!any_exit) {
        schedule_room_empty(UINT8_MAX);
    }

    s_post_room_empty_guard_until = now + pdMS_TO_TICKS(CONFIG_MCP_RADAR_CROSSING_COOLDOWN_MS);
    s_room_was_occupied = false;
}

static void process_strategy1_crossing(uint8_t i,
                                       const area_tag_t *tag,
                                       const area_config_t *cfg,
                                       const sensor_snapshot_t *snap,
                                       TickType_t now,
                                       TickType_t debounce,
                                       bool room_occupied,
                                       bool *area_notify,
                                       bool *exit_emitted,
                                       uint8_t *exit_area_id)
{
    tag_track_t *track = &s_tag_track[i];
    int active = 0;
    int static_count = 0;
    get_tag_people_count(i, cfg, snap, &active, &static_count);
    const int current = active + static_count;

    if (track->pending_total != current) {
        track->pending_total = current;
        track->count_stable_since = now;
        return;
    }

    if (track->count_stable_since == 0 ||
            (now - track->count_stable_since) < debounce) {
        return;
    }

    const int stable = track->pending_total;
    const int reported = track->reported_total < 0 ? 0 : track->reported_total;
    if (stable == reported) {
        track->count_stable_since = 0;
        return;
    }

    if (stable > reported) {
        const int delta = stable - reported;
        for (int n = 0; n < delta; n++) {
            (void)emit_area_crossing(i, true, tag->name, active, static_count, false);
        }
        *area_notify = true;
        /* 人数回升：取消因瞬时误 vacate 排队的 room_empty */
        if (room_occupied) {
            s_room_empty_pending = false;
            clear_vacated_area_notify();
        }
        ESP_LOGI(TAG, "area[%u] count up %d -> %d (+%d enter)", i, reported, stable, delta);
        track->reported_total = stable;
        track->count_stable_since = 0;
        return;
    }

    const int delta = reported - stable;
    int emitted = 0;
    for (int n = 0; n < delta; n++) {
        if (should_suppress_area_exit(room_occupied, stable, now)) {
            break;
        }
        if (emit_area_crossing(i, false, tag->name, active, static_count, false)) {
            emitted++;
        }
    }
    if (emitted == 0) {
        return;
    }

    track->reported_total = reported - emitted;
    track->count_stable_since = 0;
    if (track->reported_total == 0) {
        track->session_open = false;
    }
    ESP_LOGI(TAG, "area[%u] count down %d -> %d (-%d exit)", i, reported, track->reported_total, emitted);

    if (track->reported_total == 0 && !room_occupied) {
        *exit_emitted = true;
        *exit_area_id = i;
    } else {
        *area_notify = true;
    }
}

static void queue_area_notify(uint8_t area_id)
{
    if (area_id != UINT8_MAX && s_notify_queue != NULL) {
        (void)xQueueSend(s_notify_queue, &area_id, 0);
    }
}

static void process_snapshot(const area_config_t *cfg, const sensor_snapshot_t *snap)
{
    const TickType_t now = xTaskGetTickCount();
    const TickType_t dwell_ticks = pdMS_TO_TICKS(CONFIG_MCP_RADAR_DWELL_MS);
    const TickType_t debounce = pdMS_TO_TICKS(CONFIG_MCP_RADAR_OCC_DEBOUNCE_MS);
    const bool room_occupied = is_room_occupied(cfg, snap);
    const bool just_vacated = s_room_was_occupied && !room_occupied;
    bool exit_emitted = false;
    uint8_t exit_area_id = UINT8_MAX;
    bool area_notify[AREA_TAG_MAX] = {false};

    flush_pending_room_empty(cfg, room_occupied);

    if (just_vacated) {
        handle_room_just_vacated(cfg, snap);
        return;
    }

    if (room_occupied) {
        s_room_was_occupied = true;
        s_post_room_empty_guard_until = 0;
    }

    for (uint8_t i = 0; i < cfg->tag_count; i++) {
        const area_tag_t *tag = &cfg->tags[i];
        const area_status_t *st = &snap->areas[i];
        tag_track_t *track = &s_tag_track[i];

        int active = 0;
        int static_count = 0;
        get_tag_people_count(i, cfg, snap, &active, &static_count);

        if (tag->strategy == 2) {
            if (st->enter && !track->last_enter) {
                (void)emit_area_crossing(i, true, tag->name, active, static_count, true);
            }
            if (st->exit && !track->last_exit &&
                    !should_suppress_area_exit(room_occupied, 0, now)) {
                const bool defer_notify = !room_occupied;
                if (emit_area_crossing(i, false, tag->name, active, static_count, !defer_notify)) {
                    exit_emitted = true;
                    exit_area_id = i;
                    track->session_open = false;
                }
            }
            track->last_enter = st->enter;
            track->last_exit = st->exit;
        } else if (tag->strategy == 1) {
            process_strategy1_crossing(i, tag, cfg, snap, now, debounce, room_occupied,
                                       &area_notify[i], &exit_emitted, &exit_area_id);
        }

        /* 停留过久：区域内持续有人达到 dwell 时长（不要求 static>0，雷达常把微动标为 active） */
        if (active + static_count > 0) {
            if (track->occupied_since == 0) {
                track->occupied_since = now;
            } else if (!track->dwell_fired &&
                       (now - track->occupied_since) >= dwell_ticks) {
                emit_area_dwell(i, tag->name, active, static_count);
                track->dwell_fired = true;
            }
        } else {
            track->occupied_since = 0;
            track->dwell_fired = false;
        }
    }

    /* strategy 2：最后一人离开且已在本轮 emit exit，room_empty 延迟到下一 poll */
    if (exit_emitted && !room_occupied) {
        if (exit_area_id != UINT8_MAX) {
            notify_immediate(exit_area_id);
            schedule_room_empty(exit_area_id);
        } else {
            schedule_room_empty(UINT8_MAX);
        }
        s_room_was_occupied = false;
        s_post_room_empty_guard_until = now + pdMS_TO_TICKS(CONFIG_MCP_RADAR_CROSSING_COOLDOWN_MS);
        return;
    }

    for (uint8_t i = 0; i < cfg->tag_count && i < AREA_TAG_MAX; i++) {
        if (area_notify[i]) {
            queue_area_notify(i);
        }
    }
}

static void radar_events_watch_task(void *arg)
{
    (void)arg;
    const TickType_t interval = pdMS_TO_TICKS(CONFIG_MCP_RADAR_POLL_INTERVAL_MS);

    while (s_running) {
        if (area_config_get(&s_poll_cfg) == ESP_OK &&
            s_poll_cfg.enabled && s_poll_cfg.tag_count > 0) {
            if (sensor_data_get_snapshot(&s_poll_snap) == ESP_OK) {
                process_snapshot(&s_poll_cfg, &s_poll_snap);
            }
        } else {
            s_room_was_occupied = false;
            s_post_room_empty_guard_until = 0;
        }

        vTaskDelay(interval);
    }

    s_watch_task = NULL;
    vTaskDelete(NULL);
}

static void radar_events_notify_task(void *arg)
{
    (void)arg;

    while (s_running) {
        uint8_t area_id = MCP_EVENT_AREA_ID_GLOBAL;
        if (xQueueReceive(s_notify_queue, &area_id, pdMS_TO_TICKS(200)) != pdTRUE) {
            continue;
        }
        if (s_notify_cb != NULL) {
            s_notify_cb(area_id);
        }
    }

    /* 排空队列中剩余通知后再退出 */
    uint8_t area_id = MCP_EVENT_AREA_ID_GLOBAL;
    while (xQueueReceive(s_notify_queue, &area_id, 0) == pdTRUE) {
        if (s_notify_cb != NULL) {
            s_notify_cb(area_id);
        }
    }

    s_notify_task = NULL;
    vTaskDelete(NULL);
}

void mcp_radar_events_reset_state(void)
{
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    memset(s_tag_track, 0, sizeof(s_tag_track));
    s_room_was_occupied = false;
    s_post_room_empty_guard_until = 0;
    s_last_room_empty_at = 0;
    s_room_empty_pending = false;
    clear_vacated_area_notify();
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
    ESP_LOGI(TAG, "radar event tracking reset (area config changed)");
}

void mcp_radar_events_clear_log(void)
{
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    memset(s_event_log, 0, sizeof(s_event_log));
    s_event_log_next = 0;
    s_event_log_count = 0;
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
    ESP_LOGI(TAG, "radar event log cleared (area config changed)");
}

esp_err_t mcp_radar_events_start(mcp_radar_event_notify_cb notify_cb)
{
    if (s_running) {
        return ESP_OK;
    }

    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    memset(s_event_log, 0, sizeof(s_event_log));
    s_event_log_next = 0;
    s_event_log_count = 0;
    mcp_radar_events_reset_state();
    s_notify_cb = notify_cb;
    s_running = true;

    if (s_notify_queue == NULL) {
        s_notify_queue = xQueueCreate(8, sizeof(uint8_t));
        if (s_notify_queue == NULL) {
            s_running = false;
            s_notify_cb = NULL;
            return ESP_ERR_NO_MEM;
        }
    }

    if (xTaskCreate(radar_events_notify_task, "mcp_radar_ntf",
                    MCP_RADAR_NOTIFY_STACK, NULL, 3, &s_notify_task) != pdPASS) {
        s_running = false;
        s_notify_cb = NULL;
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(radar_events_watch_task, "mcp_radar_evt",
                    MCP_RADAR_WATCH_STACK, NULL, 4, &s_watch_task) != pdPASS) {
        s_running = false;
        while (s_notify_task != NULL) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        s_notify_cb = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "radar event watch started (poll=%dms dwell=%dms debounce=%dms log=%d)",
             CONFIG_MCP_RADAR_POLL_INTERVAL_MS, CONFIG_MCP_RADAR_DWELL_MS,
             CONFIG_MCP_RADAR_OCC_DEBOUNCE_MS, CONFIG_MCP_EVENT_LOG_SIZE);
    return ESP_OK;
}

void mcp_radar_events_stop(void)
{
    if (!s_running) {
        return;
    }

    s_running = false;
    while (s_watch_task != NULL || s_notify_task != NULL) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    s_notify_cb = NULL;
}

esp_err_t mcp_radar_events_read_feed(char **out_json)
{
    if (out_json == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    area_config_t cfg = {0};
    area_config_get(&cfg);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    char *payload = build_feed_json_locked(&cfg, 0, false);
    xSemaphoreGive(s_lock);

    if (payload == NULL) {
        return ESP_ERR_NO_MEM;
    }

    *out_json = payload;
    return ESP_OK;
}

esp_err_t mcp_radar_events_read_area(uint8_t area_id, char **out_json)
{
    if (out_json == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    area_config_t cfg = {0};
    if (area_config_get(&cfg) != ESP_OK || !cfg.enabled || area_id >= cfg.tag_count) {
        return ESP_ERR_NOT_FOUND;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    char *payload = build_feed_json_locked(&cfg, area_id, true);
    xSemaphoreGive(s_lock);

    if (payload == NULL) {
        return ESP_ERR_NO_MEM;
    }

    *out_json = payload;
    return ESP_OK;
}
