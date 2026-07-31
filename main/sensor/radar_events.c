/**
 * @file radar_events.c
 * @brief C4004 毫米波事件检测器：写入统一 device_event_bus
 */

#include "radar_events.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "c4004_sensor.h"
#include "cJSON.h"
#include "device_event_bus.h"
#include "device_event_types.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "radar_area.h"
#include "sdkconfig.h"

static const char *TAG = "radar_events";

#define EVENT_MSG_LEN 160
#define PERSON_TRACK_MAX 8

#ifndef CONFIG_MCP_RADAR_POLL_INTERVAL_MS
#define CONFIG_MCP_RADAR_POLL_INTERVAL_MS 500
#endif
#ifndef CONFIG_EVENT_RANGE_CONFIRM_MS
#define CONFIG_EVENT_RANGE_CONFIRM_MS 1000
#endif
#ifndef CONFIG_EVENT_AREA_CONFIRM_MS
#define CONFIG_EVENT_AREA_CONFIRM_MS 2000
#endif
#ifndef CONFIG_EVENT_ROOM_EMPTY_MS
#define CONFIG_EVENT_ROOM_EMPTY_MS 300000
#endif
#ifndef CONFIG_EVENT_PERSON_STATIC_MS
#define CONFIG_EVENT_PERSON_STATIC_MS 5000
#endif
#ifndef CONFIG_EVENT_PERSON_MOVING_CONFIRM_MS
#define CONFIG_EVENT_PERSON_MOVING_CONFIRM_MS 1000
#endif
#ifndef CONFIG_MCP_RADAR_CROSSING_COOLDOWN_MS
#define CONFIG_MCP_RADAR_CROSSING_COOLDOWN_MS 3000
#endif
/** 目标短暂丢帧时仍视为占用，避免确认计时被反复清零 */
#ifndef CONFIG_EVENT_RANGE_OCC_GAP_MS
#define CONFIG_EVENT_RANGE_OCC_GAP_MS 1200
#endif

typedef struct {
    bool last_enter;
    bool last_exit;
    bool last_approaching;
    bool last_leaving;
    int reported_total;
    int pending_total;
    TickType_t count_stable_since;
    TickType_t enter_confirm_since;
    TickType_t exit_confirm_since;
    TickType_t approach_confirm_since;
    TickType_t leave_confirm_since;
    bool session_open;
} tag_track_t;

typedef struct {
    bool used;
    uint8_t index;
    bool last_moving;
    bool static_fired;
    TickType_t static_since;
    TickType_t moving_confirm_since;
} person_track_t;

static TaskHandle_t s_watch_task;
static radar_event_notify_cb s_notify_cb;
static bool s_running;

static tag_track_t s_tag_track[AREA_TAG_MAX];
static person_track_t s_person[PERSON_TRACK_MAX];

/**
 * 检测范围内逐人轨迹：每人进入/离开各报一次（非「整室 0↔N」）
 * inside=true 表示已上报过 enter；丢帧 GAP 内仍视为在范围内
 */
typedef struct {
    bool used;
    uint8_t index;
    bool inside;
    TickType_t last_seen;
    TickType_t enter_since;
    TickType_t exit_since;
} range_track_t;

static range_track_t s_range_track[PERSON_TRACK_MAX];
static bool s_range_occupied; /**< 任一已确认在范围内的人 */
static bool s_room_empty_fired;
static TickType_t s_range_empty_since;
static TickType_t s_post_empty_guard_until;

static area_config_t s_poll_cfg;
static area_status_t s_poll_areas[AREA_TAG_MAX];
static uint8_t s_poll_area_count;
static c4004_target_info_t s_poll_targets[PERSON_TRACK_MAX];

/** Web 专用最近事件缓存（与总线解耦，避免被 clear/刷屏冲掉） */
#define WEB_RANGE_EVENT_MAX 16
#define WEB_RANGE_JSON_MAX  384
typedef struct {
    bool valid;
    char type[24];
    char json[WEB_RANGE_JSON_MAX];
} web_range_event_t;
static web_range_event_t s_web_range[WEB_RANGE_EVENT_MAX];
static uint8_t s_web_range_next;
static uint8_t s_web_range_count;
static SemaphoreHandle_t s_web_range_lock;

#define MCP_RADAR_WATCH_STACK 12288
#define RADAR_EVT_BUILD_TAG   "RADAR_EVT_20260731m"

static void web_range_push(const char *type, const char *area, int person_id,
                           int active, int static_count, const char *message)
{
    if (type == NULL || message == NULL) {
        return;
    }
    if (s_web_range_lock == NULL) {
        s_web_range_lock = xSemaphoreCreateMutex();
        if (s_web_range_lock == NULL) {
            return;
        }
    }

    char ts[32];
    int64_t us = esp_timer_get_time();
    snprintf(ts, sizeof(ts), "uptime+%llds", (long long)(us / 1000000LL));

    cJSON *obj = cJSON_CreateObject();
    if (obj == NULL) {
        return;
    }
    cJSON_AddStringToObject(obj, "type", type);
    cJSON_AddStringToObject(obj, "category", DEVICE_EVENT_CAT_MMWAVE);
    cJSON_AddStringToObject(obj, "timestamp", ts);
    cJSON_AddNumberToObject(obj, "area_id", DEVICE_EVENT_AREA_ID_GLOBAL);
    if (area != NULL && area[0] != '\0') {
        cJSON_AddStringToObject(obj, "area", area);
    }
    if (person_id >= 0) {
        cJSON_AddNumberToObject(obj, "person_id", person_id);
    }
    if (active >= 0) {
        cJSON_AddNumberToObject(obj, "active", active);
    }
    if (static_count >= 0) {
        cJSON_AddNumberToObject(obj, "static", static_count);
    }
    cJSON_AddStringToObject(obj, "message", message);

    char *printed = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (printed == NULL) {
        return;
    }

    xSemaphoreTake(s_web_range_lock, portMAX_DELAY);
    web_range_event_t *slot = &s_web_range[s_web_range_next];
    slot->valid = true;
    strncpy(slot->type, type, sizeof(slot->type) - 1);
    slot->type[sizeof(slot->type) - 1] = '\0';
    strncpy(slot->json, printed, sizeof(slot->json) - 1);
    slot->json[sizeof(slot->json) - 1] = '\0';
    s_web_range_next = (uint8_t)((s_web_range_next + 1U) % WEB_RANGE_EVENT_MAX);
    if (s_web_range_count < WEB_RANGE_EVENT_MAX) {
        s_web_range_count++;
    }
    xSemaphoreGive(s_web_range_lock);
    free(printed);

    ESP_LOGI(TAG, "web cache +%s (count=%u)", type, (unsigned)s_web_range_count);
}

static bool emit_mmwave(const char *type,
                        uint8_t area_id,
                        const char *area,
                        int person_id,
                        int active,
                        int static_count,
                        const char *message)
{
    /* 关键 mmwave 事件写入 Web 缓存，即使总线 emit 失败 UI 也能看到 */
    if (type != NULL &&
        (strcmp(type, DEVICE_EVENT_RANGE_ENTER) == 0 ||
         strcmp(type, DEVICE_EVENT_RANGE_EXIT) == 0 ||
         strcmp(type, DEVICE_EVENT_ROOM_EMPTY) == 0 ||
         strcmp(type, DEVICE_EVENT_APPROACH) == 0 ||
         strcmp(type, DEVICE_EVENT_AWAY) == 0 ||
         strcmp(type, DEVICE_EVENT_AREA_ENTER) == 0 ||
         strcmp(type, DEVICE_EVENT_AREA_EXIT) == 0 ||
         strcmp(type, DEVICE_EVENT_PERSON_STATIC) == 0 ||
         strcmp(type, DEVICE_EVENT_PERSON_MOVING) == 0)) {
        web_range_push(type, area, person_id, active, static_count, message);
    }

    device_event_t ev = {
        .type = type,
        .category = DEVICE_EVENT_CAT_MMWAVE,
        .area_id = area_id,
        .area = area,
        .person_id = person_id,
        .active = active,
        .static_count = static_count,
        .message = message,
        .payload = NULL,
    };
    esp_err_t err = device_event_emit(&ev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "emit %s failed: %s", type ? type : "?", esp_err_to_name(err));
        /* Web 缓存已写入：关键事件仍视为成功，避免状态与 UI 脱节 */
        if (type != NULL &&
            (strcmp(type, DEVICE_EVENT_RANGE_ENTER) == 0 ||
             strcmp(type, DEVICE_EVENT_RANGE_EXIT) == 0 ||
             strcmp(type, DEVICE_EVENT_APPROACH) == 0 ||
             strcmp(type, DEVICE_EVENT_AWAY) == 0 ||
             strcmp(type, DEVICE_EVENT_AREA_ENTER) == 0 ||
             strcmp(type, DEVICE_EVENT_AREA_EXIT) == 0 ||
             strcmp(type, DEVICE_EVENT_PERSON_STATIC) == 0 ||
             strcmp(type, DEVICE_EVENT_PERSON_MOVING) == 0)) {
            return true;
        }
        return false;
    }
    if (s_notify_cb != NULL) {
        s_notify_cb(area_id);
    }
    return true;
}

static void get_tag_people_count(uint8_t idx,
                                 const area_config_t *cfg,
                                 const area_status_t *areas,
                                 int *active,
                                 int *static_count)
{
    const area_tag_t *tag = &cfg->tags[idx];

    if (tag->strategy == 1) {
        *active = areas[idx].active;
        *static_count = areas[idx].static_count;
        return;
    }

    if (tag->strategy == 4 || tag->strategy == 5) {
        *active = 0;
        *static_count = 0;
        return;
    }

    area_rect_t rect = {0};
    if (area_coords_to_rect(tag, &rect) == ESP_OK) {
        if (c4004_sensor_count_targets_in_rect(&rect, active, static_count) != ESP_OK) {
            *active = 0;
            *static_count = 0;
        }
        return;
    }

    *active = 0;
    *static_count = 0;
}

/** 统计配置检测范围内人数（排除干扰源；未配置检测范围时用整幅 FOV） */
static int count_range_people(const c4004_target_info_t *targets, uint8_t n,
                              int *active_out, int *static_out)
{
    int active = 0;
    int st = 0;
    for (uint8_t i = 0; i < n; i++) {
        const float x = (float)targets[i].x_cm;
        const float y = (float)targets[i].y_cm;
        if (!area_point_in_detection_range(x, y)) {
            continue;
        }
        if (area_point_in_noise_zone(x, y)) {
            continue;
        }
        if (targets[i].moving) {
            active++;
        } else {
            st++;
        }
    }
    if (active_out) {
        *active_out = active;
    }
    if (static_out) {
        *static_out = st;
    }
    return active + st;
}

/** 目标所在区域名（策略 1/2）；未命中返回「未分区」 */
static const char *lookup_person_area(const area_config_t *cfg,
                                      float x_cm,
                                      float y_cm,
                                      uint8_t *out_area_id)
{
    if (out_area_id) {
        *out_area_id = DEVICE_EVENT_AREA_ID_GLOBAL;
    }
    if (cfg == NULL) {
        return "未分区";
    }
    for (uint8_t i = 0; i < cfg->tag_count; i++) {
        const area_tag_t *tag = &cfg->tags[i];
        if (tag->strategy != 1 && tag->strategy != 2) {
            continue;
        }
        area_rect_t rect = {0};
        if (area_coords_to_rect(tag, &rect) != ESP_OK) {
            continue;
        }
        if (x_cm >= rect.min_x && x_cm <= rect.max_x &&
            y_cm >= rect.min_y && y_cm <= rect.max_y) {
            if (out_area_id) {
                *out_area_id = i;
            }
            return tag->name;
        }
    }
    return "未分区";
}

static person_track_t *person_find_or_alloc(uint8_t index)
{
    for (int i = 0; i < PERSON_TRACK_MAX; i++) {
        if (s_person[i].used && s_person[i].index == index) {
            return &s_person[i];
        }
    }
    for (int i = 0; i < PERSON_TRACK_MAX; i++) {
        if (!s_person[i].used) {
            memset(&s_person[i], 0, sizeof(s_person[i]));
            s_person[i].used = true;
            s_person[i].index = index;
            s_person[i].last_moving = true;
            return &s_person[i];
        }
    }
    return NULL;
}

static const char *kinesia_desc(uint8_t k)
{
    if (k == 0) {
        return "无人";
    }
    if (k == 1) {
        return "仅呼吸静止";
    }
    if (k <= 30) {
        return "小幅肢体运动";
    }
    if (k <= 60) {
        return "缓慢体动";
    }
    return "快速体动";
}

static void process_person_motion(const area_config_t *cfg,
                                  const c4004_target_info_t *targets,
                                  uint8_t n,
                                  TickType_t now)
{
    const TickType_t static_need = pdMS_TO_TICKS(CONFIG_EVENT_PERSON_STATIC_MS);
    const TickType_t moving_need = pdMS_TO_TICKS(CONFIG_EVENT_PERSON_MOVING_CONFIRM_MS);
    bool seen[PERSON_TRACK_MAX] = {false};

    for (uint8_t t = 0; t < n; t++) {
        if (!area_point_in_detection_range((float)targets[t].x_cm, (float)targets[t].y_cm) ||
            area_point_in_noise_zone((float)targets[t].x_cm, (float)targets[t].y_cm)) {
            continue;
        }
        person_track_t *pt = person_find_or_alloc(targets[t].index);
        if (pt == NULL) {
            continue;
        }
        for (int i = 0; i < PERSON_TRACK_MAX; i++) {
            if (&s_person[i] == pt) {
                seen[i] = true;
                break;
            }
        }

        uint8_t area_id = DEVICE_EVENT_AREA_ID_GLOBAL;
        const char *area_name = lookup_person_area(cfg, (float)targets[t].x_cm,
                                                   (float)targets[t].y_cm, &area_id);
        char msg[EVENT_MSG_LEN];
        /* 官方 kinesia：<=1 视为静止体动，>=2 有肢体/躯干活动 */
        const bool has_body_motion = (targets[t].kinesia >= 2) || targets[t].moving;

        if (!has_body_motion) {
            pt->moving_confirm_since = 0;
            if (pt->static_since == 0) {
                pt->static_since = now;
            }
            if (!pt->static_fired && (now - pt->static_since) >= static_need) {
                snprintf(msg, sizeof(msg),
                         "人员#%u在%s静止%s",
                         (unsigned)targets[t].index, area_name,
                         kinesia_desc(targets[t].kinesia));
                if (emit_mmwave(DEVICE_EVENT_PERSON_STATIC, area_id, area_name,
                                (int)targets[t].index, -1, -1, msg)) {
                    ESP_LOGI(TAG,
                             "person_static person=%u area=%u \"%s\" kinesia=%u(%s) feature=0x%02X speed=%d",
                             (unsigned)targets[t].index, (unsigned)area_id, area_name,
                             targets[t].kinesia, kinesia_desc(targets[t].kinesia),
                             targets[t].feature, (int)targets[t].speed_cm_s);
                }
                pt->static_fired = true;
            }
            pt->last_moving = false;
        } else {
            pt->static_since = 0;
            if (pt->static_fired) {
                if (pt->moving_confirm_since == 0) {
                    pt->moving_confirm_since = now;
                } else if ((now - pt->moving_confirm_since) >= moving_need) {
                    snprintf(msg, sizeof(msg),
                             "人员#%u在%s有%s",
                             (unsigned)targets[t].index, area_name,
                             kinesia_desc(targets[t].kinesia));
                    if (emit_mmwave(DEVICE_EVENT_PERSON_MOVING, area_id, area_name,
                                    (int)targets[t].index, -1, -1, msg)) {
                        ESP_LOGI(TAG,
                                 "person_moving person=%u area=%u \"%s\" kinesia=%u(%s) feature=0x%02X speed=%d",
                                 (unsigned)targets[t].index, (unsigned)area_id, area_name,
                                 targets[t].kinesia, kinesia_desc(targets[t].kinesia),
                                 targets[t].feature, (int)targets[t].speed_cm_s);
                    }
                    pt->static_fired = false;
                    pt->moving_confirm_since = 0;
                }
            } else {
                pt->moving_confirm_since = 0;
            }
            pt->last_moving = true;
        }
    }

    for (int i = 0; i < PERSON_TRACK_MAX; i++) {
        if (s_person[i].used && !seen[i]) {
            memset(&s_person[i], 0, sizeof(s_person[i]));
        }
    }
}

static range_track_t *range_track_find(uint8_t index)
{
    for (int i = 0; i < PERSON_TRACK_MAX; i++) {
        if (s_range_track[i].used && s_range_track[i].index == index) {
            return &s_range_track[i];
        }
    }
    return NULL;
}

static range_track_t *range_track_alloc(uint8_t index)
{
    range_track_t *t = range_track_find(index);
    if (t != NULL) {
        return t;
    }
    for (int i = 0; i < PERSON_TRACK_MAX; i++) {
        if (!s_range_track[i].used) {
            memset(&s_range_track[i], 0, sizeof(s_range_track[i]));
            s_range_track[i].used = true;
            s_range_track[i].index = index;
            return &s_range_track[i];
        }
    }
    return NULL;
}

/**
 * 逐人检测范围事件：
 * - 某人进入检测范围并稳定 confirm → range_enter（每人一次）
 * - 某人离开检测范围并稳定 confirm → range_exit（每人一次）
 * 短暂丢帧（GAP）不打断在场判定。
 */
static void process_range_events(const c4004_target_info_t *targets, uint8_t n, TickType_t now)
{
    const TickType_t confirm = pdMS_TO_TICKS(CONFIG_EVENT_RANGE_CONFIRM_MS);
    const TickType_t gap = pdMS_TO_TICKS(CONFIG_EVENT_RANGE_OCC_GAP_MS);
    const TickType_t empty_need = pdMS_TO_TICKS(CONFIG_EVENT_ROOM_EMPTY_MS);

    int active = 0;
    int static_count = 0;
    (void)count_range_people(targets, n, &active, &static_count);

    for (uint8_t i = 0; i < n; i++) {
        const float x = (float)targets[i].x_cm;
        const float y = (float)targets[i].y_cm;
        if (!area_point_in_detection_range(x, y) || area_point_in_noise_zone(x, y)) {
            continue;
        }

        range_track_t *tr = range_track_alloc(targets[i].index);
        if (tr == NULL) {
            continue;
        }

        tr->last_seen = now;
        tr->exit_since = 0;

        if (!tr->inside) {
            if (tr->enter_since == 0) {
                tr->enter_since = now;
            }
            if ((now - tr->enter_since) >= confirm) {
                char msg[EVENT_MSG_LEN];
                snprintf(msg, sizeof(msg),
                         "人员#%u进入检测范围（活动%d人，静止%d人）",
                         (unsigned)tr->index, active, static_count);
                if (emit_mmwave(DEVICE_EVENT_RANGE_ENTER, DEVICE_EVENT_AREA_ID_GLOBAL, "检测范围",
                                (int)tr->index, active, static_count, msg)) {
                    tr->inside = true;
                    tr->enter_since = 0;
                    s_room_empty_fired = false;
                    s_range_empty_since = 0;
                    s_post_empty_guard_until = 0;
                    ESP_LOGI(TAG, "range_enter person=%u in_range=%d",
                             (unsigned)tr->index, active + static_count);
                }
            }
        } else {
            tr->enter_since = 0;
        }
    }

    for (int i = 0; i < PERSON_TRACK_MAX; i++) {
        range_track_t *tr = &s_range_track[i];
        if (!tr->used) {
            continue;
        }

        const bool soft_here =
            (tr->last_seen != 0) && ((now - tr->last_seen) <= gap);

        if (soft_here) {
            continue;
        }

        if (!tr->inside) {
            /* 未确认进入就消失：丢弃轨迹 */
            memset(tr, 0, sizeof(*tr));
            continue;
        }

        if (tr->exit_since == 0) {
            tr->exit_since = now;
        }
        if ((now - tr->exit_since) >= confirm) {
            char msg[EVENT_MSG_LEN];
            snprintf(msg, sizeof(msg), "人员#%u离开检测范围", (unsigned)tr->index);
            if (emit_mmwave(DEVICE_EVENT_RANGE_EXIT, DEVICE_EVENT_AREA_ID_GLOBAL, "检测范围",
                            (int)tr->index, active, static_count, msg)) {
                ESP_LOGI(TAG, "range_exit person=%u", (unsigned)tr->index);
                s_post_empty_guard_until =
                    now + pdMS_TO_TICKS(CONFIG_MCP_RADAR_CROSSING_COOLDOWN_MS);
                memset(tr, 0, sizeof(*tr));
            }
        }
    }

    bool any_inside = false;
    bool any_soft = false;
    for (int i = 0; i < PERSON_TRACK_MAX; i++) {
        if (!s_range_track[i].used) {
            continue;
        }
        if (s_range_track[i].inside) {
            any_inside = true;
        }
        if (s_range_track[i].last_seen != 0 &&
            (now - s_range_track[i].last_seen) <= gap) {
            any_soft = true;
        }
    }
    s_range_occupied = any_inside;

    if (any_inside || any_soft) {
        s_range_empty_since = 0;
    } else {
        if (s_range_empty_since == 0) {
            s_range_empty_since = now;
        }
        if (!s_room_empty_fired && (now - s_range_empty_since) >= empty_need) {
            emit_mmwave(DEVICE_EVENT_ROOM_EMPTY, DEVICE_EVENT_AREA_ID_GLOBAL, "检测范围",
                        -1, -1, -1, "检测范围无人");
            s_room_empty_fired = true;
        }
    }
}

static bool tag_rect_cm(const area_tag_t *tag, float *min_x, float *max_x,
                        float *min_y, float *max_y, float *cx, float *cy);

#ifndef CONFIG_EVENT_BOUNDARY_COOLDOWN_MS
#define CONFIG_EVENT_BOUNDARY_COOLDOWN_MS 800
#endif
#ifndef CONFIG_EVENT_BOUNDARY_CONFIRM_MS
#define CONFIG_EVENT_BOUNDARY_CONFIRM_MS 400
#endif

typedef struct {
    bool used;
    uint8_t person;
    bool inside;
    TickType_t last_seen;
    TickType_t enter_since;
    TickType_t exit_since;
} boundary_person_t;

static boundary_person_t s_bd_person[AREA_TAG_MAX][PERSON_TRACK_MAX];
static TickType_t s_enter_cooldown_until[AREA_TAG_MAX];
static TickType_t s_exit_cooldown_until[AREA_TAG_MAX];

static bool emit_area_crossing(uint8_t area_id,
                               bool is_enter,
                               const char *area_name,
                               int person_id,
                               int active,
                               int static_count,
                               TickType_t now,
                               bool with_cooldown)
{
    if (with_cooldown) {
        TickType_t *cool = is_enter ? &s_enter_cooldown_until[area_id]
                                    : &s_exit_cooldown_until[area_id];
        if (*cool != 0 && now < *cool) {
            return false;
        }
    }

    char message[EVENT_MSG_LEN];
    if (is_enter) {
        if (person_id >= 0) {
            snprintf(message, sizeof(message),
                     "人员#%d进入%s，该区域有%d人活动，%d人静止",
                     person_id, area_name, active, static_count);
        } else {
            snprintf(message, sizeof(message),
                     "有人进入%s区域，该区域有%d人活动，%d人静止",
                     area_name, active, static_count);
        }
    } else if (person_id >= 0) {
        snprintf(message, sizeof(message),
                 "人员#%d离开%s，该区域有%d人活动，%d人静止",
                 person_id, area_name, active, static_count);
    } else {
        snprintf(message, sizeof(message),
                 "有人离开%s区域，该区域有%d人活动，%d人静止",
                 area_name, active, static_count);
    }

    if (!emit_mmwave(is_enter ? DEVICE_EVENT_AREA_ENTER : DEVICE_EVENT_AREA_EXIT,
                     area_id, area_name, person_id, active, static_count, message)) {
        return false;
    }

    if (with_cooldown) {
        TickType_t *cool = is_enter ? &s_enter_cooldown_until[area_id]
                                    : &s_exit_cooldown_until[area_id];
        *cool = now + pdMS_TO_TICKS(CONFIG_EVENT_BOUNDARY_COOLDOWN_MS);
    }

    ESP_LOGI(TAG, "%s area=%u \"%s\" person=%d",
             is_enter ? "area_enter" : "area_exit",
             (unsigned)area_id, area_name, person_id);

    if (is_enter) {
        s_post_empty_guard_until = 0;
        s_tag_track[area_id].session_open = true;
    } else {
        s_tag_track[area_id].session_open = false;
    }
    return true;
}

static bool should_suppress_area_exit(bool range_occupied, int stable_count, TickType_t now)
{
    if (stable_count == 0) {
        return false;
    }
    return !range_occupied &&
           s_post_empty_guard_until != 0 &&
           now < s_post_empty_guard_until;
}

static void process_strategy1_crossing(uint8_t i,
                                       const area_tag_t *tag,
                                       const area_config_t *cfg,
                                       const area_status_t *areas,
                                       TickType_t now,
                                       TickType_t debounce,
                                       bool range_occupied)
{
    tag_track_t *track = &s_tag_track[i];
    int active = 0;
    int static_count = 0;
    get_tag_people_count(i, cfg, areas, &active, &static_count);
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
            (void)emit_area_crossing(i, true, tag->name, -1, active, static_count, now, false);
        }
        track->reported_total = stable;
        track->count_stable_since = 0;
        return;
    }

    const int delta = reported - stable;
    int emitted = 0;
    for (int n = 0; n < delta; n++) {
        if (should_suppress_area_exit(range_occupied, stable, now)) {
            break;
        }
        if (emit_area_crossing(i, false, tag->name, -1, active, static_count, now, false)) {
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
}

/**
 * strategy=2 边界/边缘门：
 * 1) C4004 TAG_REPORT 进出脉冲上升沿立即上报
 * 2) 轨迹穿越标签矩形（外→内 enter，内→外 exit）兜底
 */
static void process_strategy2_crossing(uint8_t i,
                                       const area_tag_t *tag,
                                       const area_status_t *st,
                                       const c4004_target_info_t *targets,
                                       uint8_t tcount,
                                       int active,
                                       int static_count,
                                       TickType_t now,
                                       TickType_t confirm,
                                       bool range_occupied)
{
    tag_track_t *track = &s_tag_track[i];

    /* ---- 路径 A：模块主动上报 ---- */
    if (st->enter && !track->last_enter) {
        (void)emit_area_crossing(i, true, tag->name, -1, active, static_count, now, true);
        track->reported_total = 1;
    }
    if (st->exit && !track->last_exit) {
        if (!should_suppress_area_exit(range_occupied, 0, now)) {
            (void)emit_area_crossing(i, false, tag->name, -1, active, static_count, now, true);
            track->reported_total = 0;
        }
    }
    track->last_enter = st->enter;
    track->last_exit = st->exit;

    /* ---- 路径 B：轨迹进出标签矩形 ---- */
    float min_x, max_x, min_y, max_y, cx, cy;
    if (!tag_rect_cm(tag, &min_x, &max_x, &min_y, &max_y, &cx, &cy) || targets == NULL) {
        return;
    }
    (void)cx;
    (void)cy;

    bool seen[PERSON_TRACK_MAX] = {false};
    const TickType_t gap = pdMS_TO_TICKS(CONFIG_EVENT_RANGE_OCC_GAP_MS);

    for (uint8_t t = 0; t < tcount; t++) {
        const float x = (float)targets[t].x_cm;
        const float y = (float)targets[t].y_cm;
        if (area_point_in_noise_zone(x, y)) {
            continue;
        }

        const bool in_rect = (x >= min_x && x <= max_x && y >= min_y && y <= max_y);

        boundary_person_t *bp = NULL;
        for (int k = 0; k < PERSON_TRACK_MAX; k++) {
            if (s_bd_person[i][k].used && s_bd_person[i][k].person == targets[t].index) {
                bp = &s_bd_person[i][k];
                seen[k] = true;
                break;
            }
        }
        if (bp == NULL) {
            for (int k = 0; k < PERSON_TRACK_MAX; k++) {
                if (!s_bd_person[i][k].used) {
                    bp = &s_bd_person[i][k];
                    memset(bp, 0, sizeof(*bp));
                    bp->used = true;
                    bp->person = targets[t].index;
                    bp->inside = in_rect;
                    bp->last_seen = now;
                    seen[k] = true;
                    break;
                }
            }
        }
        if (bp == NULL) {
            continue;
        }

        bp->last_seen = now;

        if (in_rect) {
            bp->exit_since = 0;
            if (!bp->inside) {
                if (bp->enter_since == 0) {
                    bp->enter_since = now;
                } else if ((now - bp->enter_since) >= confirm) {
                    if (emit_area_crossing(i, true, tag->name, (int)bp->person,
                                           active, static_count, now, true)) {
                        bp->inside = true;
                        track->reported_total = 1;
                    }
                    bp->enter_since = 0;
                }
            } else {
                bp->enter_since = 0;
            }
        } else {
            bp->enter_since = 0;
            if (bp->inside) {
                if (bp->exit_since == 0) {
                    bp->exit_since = now;
                } else if ((now - bp->exit_since) >= confirm) {
                    if (!should_suppress_area_exit(range_occupied, 0, now) &&
                        emit_area_crossing(i, false, tag->name, (int)bp->person,
                                           active, static_count, now, true)) {
                        bp->inside = false;
                        track->reported_total = 0;
                    }
                    bp->exit_since = 0;
                }
            } else {
                bp->exit_since = 0;
            }
        }
    }

    for (int k = 0; k < PERSON_TRACK_MAX; k++) {
        boundary_person_t *bp = &s_bd_person[i][k];
        if (!bp->used) {
            continue;
        }
        if (seen[k]) {
            continue;
        }
        /* 短暂丢目标：仍按上次 inside 状态；超时再清槽 */
        if ((now - bp->last_seen) < gap) {
            continue;
        }
        if (bp->inside) {
            if (bp->exit_since == 0) {
                bp->exit_since = now;
            } else if ((now - bp->exit_since) >= confirm) {
                if (!should_suppress_area_exit(range_occupied, 0, now)) {
                    (void)emit_area_crossing(i, false, tag->name, (int)bp->person,
                                             active, static_count, now, true);
                    track->reported_total = 0;
                }
                memset(bp, 0, sizeof(*bp));
            }
        } else {
            memset(bp, 0, sizeof(*bp));
        }
    }
}

#ifndef CONFIG_EVENT_APPROACH_COOLDOWN_MS
#define CONFIG_EVENT_APPROACH_COOLDOWN_MS 800
#endif
/** 到标签矩形边界的累计位移达到该值触发靠近/远离（cm） */
#ifndef CONFIG_EVENT_APPROACH_TRIGGER_CM
#define CONFIG_EVENT_APPROACH_TRIGGER_CM 20
#endif
/** 在标签外多远仍跟踪靠近/远离（到矩形距离，cm）；默认 0.5m */
#ifndef CONFIG_EVENT_APPROACH_WATCH_CM
#define CONFIG_EVENT_APPROACH_WATCH_CM 50
#endif

typedef struct {
    bool used;
    uint8_t person;
    float last_dist;
    float cum_delta; /**< >0 远离区域累计, <0 靠近区域累计 */
} approach_person_t;

static approach_person_t s_ap_person[AREA_TAG_MAX][PERSON_TRACK_MAX];
static TickType_t s_approach_cooldown_until[AREA_TAG_MAX];
static TickType_t s_away_cooldown_until[AREA_TAG_MAX];

static bool tag_rect_cm(const area_tag_t *tag, float *min_x, float *max_x,
                        float *min_y, float *max_y, float *cx, float *cy)
{
    if (tag == NULL) {
        return false;
    }
    float x0 = tag->coords[0][0], x1 = tag->coords[0][0];
    float y0 = tag->coords[0][1], y1 = tag->coords[0][1];
    for (int p = 1; p < 4; p++) {
        if (tag->coords[p][0] < x0) {
            x0 = tag->coords[p][0];
        }
        if (tag->coords[p][0] > x1) {
            x1 = tag->coords[p][0];
        }
        if (tag->coords[p][1] < y0) {
            y0 = tag->coords[p][1];
        }
        if (tag->coords[p][1] > y1) {
            y1 = tag->coords[p][1];
        }
    }
    if ((x1 - x0) < AREA_RECT_MIN_SIZE_CM || (y1 - y0) < AREA_RECT_MIN_SIZE_CM) {
        return false;
    }
    if (min_x) {
        *min_x = x0;
    }
    if (max_x) {
        *max_x = x1;
    }
    if (min_y) {
        *min_y = y0;
    }
    if (max_y) {
        *max_y = y1;
    }
    if (cx) {
        *cx = (x0 + x1) * 0.5f;
    }
    if (cy) {
        *cy = (y0 + y1) * 0.5f;
    }
    return true;
}

/** 点到轴对齐矩形的距离；在矩形内为 0 */
static float dist_to_rect_cm(float x, float y,
                             float min_x, float max_x, float min_y, float max_y)
{
    float dx = 0.0f;
    float dy = 0.0f;
    if (x < min_x) {
        dx = min_x - x;
    } else if (x > max_x) {
        dx = x - max_x;
    }
    if (y < min_y) {
        dy = min_y - y;
    } else if (y > max_y) {
        dy = y - max_y;
    }
    if (dx == 0.0f) {
        return dy;
    }
    if (dy == 0.0f) {
        return dx;
    }
    return sqrtf(dx * dx + dy * dy);
}

static bool emit_approach_away(uint8_t area_id, const char *area_name, bool is_approach,
                               int person_id, TickType_t now)
{
    TickType_t *cool = is_approach ? &s_approach_cooldown_until[area_id]
                                   : &s_away_cooldown_until[area_id];
    if (*cool != 0 && now < *cool) {
        return false;
    }

    char msg[EVENT_MSG_LEN];
    if (is_approach) {
        if (person_id >= 0) {
            snprintf(msg, sizeof(msg), "人员#%d靠近%s", person_id, area_name);
        } else {
            snprintf(msg, sizeof(msg), "有人靠近%s", area_name);
        }
    } else if (person_id >= 0) {
        snprintf(msg, sizeof(msg), "人员#%d远离%s", person_id, area_name);
    } else {
        snprintf(msg, sizeof(msg), "有人远离%s", area_name);
    }

    if (!emit_mmwave(is_approach ? DEVICE_EVENT_APPROACH : DEVICE_EVENT_AWAY,
                     area_id, area_name, person_id, -1, -1, msg)) {
        return false;
    }
    *cool = now + pdMS_TO_TICKS(CONFIG_EVENT_APPROACH_COOLDOWN_MS);
    ESP_LOGI(TAG, "%s area=%u \"%s\" person=%d",
             is_approach ? "approach" : "away", (unsigned)area_id, area_name, person_id);
    return true;
}

/**
 * strategy=3 靠近/远离：
 * 1) C4004 TAG_REPORT 脉冲上升沿立即上报
 * 2) 轨迹：到标签矩形边界的距离变化（人在区外靠近也会触发；区内 dist=0）
 */
static void process_strategy3_approach_away(uint8_t i,
                                            const area_tag_t *tag,
                                            const area_status_t *st,
                                            const c4004_target_info_t *targets,
                                            uint8_t tcount,
                                            TickType_t now,
                                            TickType_t traj_confirm)
{
    (void)traj_confirm;
    tag_track_t *track = &s_tag_track[i];

    /* ---- 路径 A：模块主动上报 ---- */
    if (st->approaching && !track->last_approaching) {
        (void)emit_approach_away(i, tag->name, true, -1, now);
    }
    if (st->leaving && !track->last_leaving) {
        (void)emit_approach_away(i, tag->name, false, -1, now);
    }
    track->last_approaching = st->approaching;
    track->last_leaving = st->leaving;

    /* ---- 路径 B：到矩形距离的累计变化 ---- */
    float min_x, max_x, min_y, max_y, cx, cy;
    if (!tag_rect_cm(tag, &min_x, &max_x, &min_y, &max_y, &cx, &cy) || targets == NULL) {
        return;
    }
    (void)cx;
    (void)cy;

    bool seen[PERSON_TRACK_MAX] = {false};
    const float trigger = (float)CONFIG_EVENT_APPROACH_TRIGGER_CM;
    const float watch = (float)CONFIG_EVENT_APPROACH_WATCH_CM;

    for (uint8_t t = 0; t < tcount; t++) {
        const float x = (float)targets[t].x_cm;
        const float y = (float)targets[t].y_cm;
        if (area_point_in_noise_zone(x, y)) {
            continue;
        }

        const float dist = dist_to_rect_cm(x, y, min_x, max_x, min_y, max_y);
        /* 太远不跟踪；已在跟踪且走出 watch 则下面 seen 清槽 */
        if (dist > watch) {
            continue;
        }

        approach_person_t *ap = NULL;
        for (int k = 0; k < PERSON_TRACK_MAX; k++) {
            if (s_ap_person[i][k].used && s_ap_person[i][k].person == targets[t].index) {
                ap = &s_ap_person[i][k];
                seen[k] = true;
                break;
            }
        }
        if (ap == NULL) {
            for (int k = 0; k < PERSON_TRACK_MAX; k++) {
                if (!s_ap_person[i][k].used) {
                    ap = &s_ap_person[i][k];
                    memset(ap, 0, sizeof(*ap));
                    ap->used = true;
                    ap->person = targets[t].index;
                    ap->last_dist = -1.0f;
                    seen[k] = true;
                    break;
                }
            }
        }
        if (ap == NULL) {
            continue;
        }

        if (ap->last_dist < 0.0f) {
            ap->last_dist = dist;
            ap->cum_delta = 0.0f;
            continue;
        }

        const float prev = ap->last_dist;
        ap->cum_delta += (dist - prev);
        ap->last_dist = dist;

        /* 持续停在区内 dist 恒 0：清累计，避免原地抖动；靠近应在进区前完成 */
        if (dist <= 0.0f && prev <= 0.0f) {
            ap->cum_delta = 0.0f;
        }

        if (ap->cum_delta <= -trigger) {
            if (emit_approach_away(i, tag->name, true, (int)ap->person, now)) {
                ap->cum_delta = 0.0f;
            } else {
                ap->cum_delta = -trigger;
            }
        } else if (ap->cum_delta >= trigger) {
            if (emit_approach_away(i, tag->name, false, (int)ap->person, now)) {
                ap->cum_delta = 0.0f;
            } else {
                ap->cum_delta = trigger;
            }
        }
    }

    for (int k = 0; k < PERSON_TRACK_MAX; k++) {
        if (s_ap_person[i][k].used && !seen[k]) {
            memset(&s_ap_person[i][k], 0, sizeof(s_ap_person[i][k]));
        }
    }
}

static void process_area_events(const area_config_t *cfg,
                                const area_status_t *areas,
                                const c4004_target_info_t *targets,
                                uint8_t tcount,
                                bool range_occupied,
                                TickType_t now)
{
    const TickType_t confirm = pdMS_TO_TICKS(CONFIG_EVENT_AREA_CONFIRM_MS);
    const TickType_t bd_confirm = pdMS_TO_TICKS(CONFIG_EVENT_BOUNDARY_CONFIRM_MS);
    const TickType_t ap_confirm = pdMS_TO_TICKS(CONFIG_EVENT_RANGE_CONFIRM_MS);

    for (uint8_t i = 0; i < cfg->tag_count; i++) {
        const area_tag_t *tag = &cfg->tags[i];
        const area_status_t *st = &areas[i];
        int active = 0;
        int static_count = 0;
        get_tag_people_count(i, cfg, areas, &active, &static_count);

        if (tag->strategy == 2) {
            process_strategy2_crossing(i, tag, st, targets, tcount, active, static_count,
                                       now, bd_confirm, range_occupied);
        } else if (tag->strategy == 1) {
            process_strategy1_crossing(i, tag, cfg, areas, now, confirm, range_occupied);
        } else if (tag->strategy == 3) {
            process_strategy3_approach_away(i, tag, st, targets, tcount, now, ap_confirm);
        }
    }
}

static void process_snapshot(const area_config_t *cfg, const area_status_t *areas)
{
    const TickType_t now = xTaskGetTickCount();

    uint8_t tcount = 0;
    if (c4004_sensor_is_ready()) {
        (void)c4004_sensor_refresh_targets();
        (void)c4004_sensor_get_targets(s_poll_targets, PERSON_TRACK_MAX, &tcount);
    }

    int active = 0;
    int static_count = 0;
    const int range_people = count_range_people(s_poll_targets, tcount, &active, &static_count);
    int tracked_inside = 0;
    for (int i = 0; i < PERSON_TRACK_MAX; i++) {
        if (s_range_track[i].used && s_range_track[i].inside) {
            tracked_inside++;
        }
    }

    static TickType_t s_last_dbg;
    if (s_last_dbg == 0 || (now - s_last_dbg) >= pdMS_TO_TICKS(5000)) {
        s_last_dbg = now;
        ESP_LOGI(TAG,
                 "range watch: ready=%d targets=%u in_range=%d inside=%d web_ev=%u",
                 (int)c4004_sensor_is_ready(), (unsigned)tcount, range_people,
                 tracked_inside, (unsigned)s_web_range_count);
        for (uint8_t t = 0; t < tcount && t < 8; t++) {
            ESP_LOGI(TAG,
                     "  motion person=%u kinesia=%u(%s) feature=0x%02X speed=%d cm/s moving=%d pos=(%d,%d)cm",
                     (unsigned)s_poll_targets[t].index,
                     s_poll_targets[t].kinesia,
                     kinesia_desc(s_poll_targets[t].kinesia),
                     s_poll_targets[t].feature,
                     (int)s_poll_targets[t].speed_cm_s,
                     (int)s_poll_targets[t].moving,
                     (int)s_poll_targets[t].x_cm,
                     (int)s_poll_targets[t].y_cm);
        }
        if (cfg != NULL && areas != NULL) {
            for (uint8_t i = 0; i < cfg->tag_count; i++) {
                if (cfg->tags[i].strategy == 2) {
                    ESP_LOGI(TAG, "  boundary_tag[%u] \"%s\" enter=%d exit=%d",
                             (unsigned)i, cfg->tags[i].name,
                             (int)areas[i].enter, (int)areas[i].exit);
                } else if (cfg->tags[i].strategy == 3) {
                    ESP_LOGI(TAG, "  approach_tag[%u] \"%s\" approaching=%d leaving=%d",
                             (unsigned)i, cfg->tags[i].name,
                             (int)areas[i].approaching, (int)areas[i].leaving);
                    float min_x, max_x, min_y, max_y, cx, cy;
                    if (!tag_rect_cm(&cfg->tags[i], &min_x, &max_x, &min_y, &max_y, &cx, &cy)) {
                        continue;
                    }
                    (void)cx;
                    (void)cy;
                    float best = 1.0e9f;
                    int best_id = -1;
                    for (uint8_t t = 0; t < tcount; t++) {
                        const float d = dist_to_rect_cm((float)s_poll_targets[t].x_cm,
                                                        (float)s_poll_targets[t].y_cm,
                                                        min_x, max_x, min_y, max_y);
                        if (d < best) {
                            best = d;
                            best_id = (int)s_poll_targets[t].index;
                        }
                    }
                    if (best_id >= 0) {
                        ESP_LOGI(TAG, "    nearest person=%d dist_to_tag=%.0fcm (need move >=%dcm)",
                                 best_id, best, CONFIG_EVENT_APPROACH_TRIGGER_CM);
                    } else {
                        ESP_LOGI(TAG, "    no targets to track");
                    }
                }
            }
        }
        if (tcount > 0 && range_people == 0) {
            for (uint8_t i = 0; i < tcount && i < 3; i++) {
                ESP_LOGW(TAG, "  target[%u] (%d,%d)cm outside detection_range or in noise",
                         (unsigned)s_poll_targets[i].index,
                         (int)s_poll_targets[i].x_cm, (int)s_poll_targets[i].y_cm);
            }
        }
    }

    process_range_events(s_poll_targets, tcount, now);
    process_person_motion(cfg, s_poll_targets, tcount, now);

    if (cfg != NULL && cfg->tag_count > 0 && areas != NULL) {
        process_area_events(cfg, areas, s_poll_targets, tcount, s_range_occupied, now);
    }
}

static void radar_events_watch_task(void *arg)
{
    (void)arg;
    const TickType_t interval = pdMS_TO_TICKS(CONFIG_MCP_RADAR_POLL_INTERVAL_MS);
    unsigned loop = 0;

    ESP_LOGE(TAG, "%s watch task running", RADAR_EVT_BUILD_TAG);

    while (s_running) {
        const bool have_cfg = (area_config_get(&s_poll_cfg) == ESP_OK);
        if (have_cfg && s_poll_cfg.tag_count > 0 &&
            radar_area_get_status(s_poll_areas, &s_poll_area_count) == ESP_OK) {
            process_snapshot(&s_poll_cfg, s_poll_areas);
        } else {
            process_snapshot(have_cfg ? &s_poll_cfg : NULL, NULL);
        }
        if (loop < 20U) {
            ESP_LOGI(TAG, "%s loop=%u ready=%d web_ev=%u occ=%d",
                     RADAR_EVT_BUILD_TAG, loop,
                     (int)c4004_sensor_is_ready(),
                     (unsigned)s_web_range_count,
                     (int)s_range_occupied);
            loop++;
        }
        vTaskDelay(interval);
    }

    s_watch_task = NULL;
    vTaskDelete(NULL);
}

void radar_events_reset_state(void)
{
    memset(s_tag_track, 0, sizeof(s_tag_track));
    memset(s_person, 0, sizeof(s_person));
    memset(s_range_track, 0, sizeof(s_range_track));
    memset(s_ap_person, 0, sizeof(s_ap_person));
    memset(s_approach_cooldown_until, 0, sizeof(s_approach_cooldown_until));
    memset(s_away_cooldown_until, 0, sizeof(s_away_cooldown_until));
    memset(s_bd_person, 0, sizeof(s_bd_person));
    memset(s_enter_cooldown_until, 0, sizeof(s_enter_cooldown_until));
    memset(s_exit_cooldown_until, 0, sizeof(s_exit_cooldown_until));
    s_range_occupied = false;
    s_room_empty_fired = false;
    s_range_empty_since = 0;
    s_post_empty_guard_until = 0;
    ESP_LOGI(TAG, "radar event tracking reset");
}

void radar_events_clear_log(void)
{
    device_event_clear();
    ESP_LOGI(TAG, "device event log cleared");
}

esp_err_t radar_events_start(radar_event_notify_cb notify_cb)
{
    if (s_running) {
        return ESP_OK;
    }

    esp_err_t err = device_event_bus_init();
    if (err != ESP_OK) {
        return err;
    }

    radar_events_reset_state();
    s_notify_cb = notify_cb;
    s_running = true;

    if (xTaskCreate(radar_events_watch_task, "mcp_radar_evt",
                    MCP_RADAR_WATCH_STACK, NULL, 4, &s_watch_task) != pdPASS) {
        s_running = false;
        s_notify_cb = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGE(TAG,
             "%s started ok (poll=%dms range_confirm=%dms)",
             RADAR_EVT_BUILD_TAG,
             CONFIG_MCP_RADAR_POLL_INTERVAL_MS,
             CONFIG_EVENT_RANGE_CONFIRM_MS);
    return ESP_OK;
}

void radar_events_stop(void)
{
    if (!s_running) {
        return;
    }
    s_running = false;
    while (s_watch_task != NULL) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    s_notify_cb = NULL;
}

esp_err_t radar_events_read_feed(char **out_json)
{
    return device_event_read_feed(out_json);
}

esp_err_t radar_events_read_area(uint8_t area_id, char **out_json)
{
    return device_event_read_area(area_id, out_json);
}

bool radar_events_range_occupied(void)
{
    return s_range_occupied;
}

esp_err_t radar_events_fill_web_recent(cJSON *out_array, int max_count)
{
    if (out_array == NULL || !cJSON_IsArray(out_array)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_web_range_lock == NULL || s_web_range_count == 0) {
        return ESP_OK;
    }

    xSemaphoreTake(s_web_range_lock, portMAX_DELAY);
    uint8_t start = (uint8_t)((s_web_range_next + WEB_RANGE_EVENT_MAX - s_web_range_count) %
                              WEB_RANGE_EVENT_MAX);
    uint8_t matched[WEB_RANGE_EVENT_MAX];
    uint8_t matched_n = 0;
    for (uint8_t n = s_web_range_count; n > 0; n--) {
        uint8_t idx = (uint8_t)((start + n - 1U) % WEB_RANGE_EVENT_MAX);
        if (!s_web_range[idx].valid) {
            continue;
        }
        matched[matched_n++] = idx;
        if (max_count > 0 && matched_n >= (uint8_t)max_count) {
            break;
        }
    }
    for (uint8_t i = matched_n; i > 0; i--) {
        const web_range_event_t *rec = &s_web_range[matched[i - 1U]];
        cJSON *item = cJSON_Parse(rec->json);
        if (item != NULL) {
            cJSON_AddItemToArray(out_array, item);
        }
    }
    xSemaphoreGive(s_web_range_lock);
    return ESP_OK;
}
