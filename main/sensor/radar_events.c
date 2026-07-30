/**
 * @file radar_events.c
 * @brief C4004 毫米波事件检测器：写入统一 device_event_bus
 */

#include "radar_events.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "c4004_sensor.h"
#include "device_event_bus.h"
#include "device_event_types.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
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
#define CONFIG_EVENT_RANGE_CONFIRM_MS 2000
#endif
#ifndef CONFIG_EVENT_AREA_CONFIRM_MS
#define CONFIG_EVENT_AREA_CONFIRM_MS 2000
#endif
#ifndef CONFIG_EVENT_ROOM_EMPTY_MS
#define CONFIG_EVENT_ROOM_EMPTY_MS 300000
#endif
#ifndef CONFIG_EVENT_PERSON_STATIC_MS
#define CONFIG_EVENT_PERSON_STATIC_MS 60000
#endif
#ifndef CONFIG_EVENT_PERSON_MOVING_CONFIRM_MS
#define CONFIG_EVENT_PERSON_MOVING_CONFIRM_MS 2000
#endif
#ifndef CONFIG_MCP_RADAR_CROSSING_COOLDOWN_MS
#define CONFIG_MCP_RADAR_CROSSING_COOLDOWN_MS 3000
#endif

typedef struct {
    bool last_enter;
    bool last_exit;
    int reported_total;
    int pending_total;
    TickType_t count_stable_since;
    TickType_t enter_confirm_since;
    TickType_t exit_confirm_since;
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

/** 探测范围占用（排除干扰源） */
static bool s_range_occupied;
static bool s_range_pending_occ;
static TickType_t s_range_stable_since;
static bool s_room_empty_fired;
static TickType_t s_range_empty_since;
static TickType_t s_post_empty_guard_until;

static area_config_t s_poll_cfg;
static area_status_t s_poll_areas[AREA_TAG_MAX];
static uint8_t s_poll_area_count;
static c4004_target_info_t s_poll_targets[PERSON_TRACK_MAX];

#define MCP_RADAR_WATCH_STACK 8192

static void emit_mmwave(const char *type,
                        uint8_t area_id,
                        const char *area,
                        int person_id,
                        int active,
                        int static_count,
                        const char *message)
{
    device_event_t ev = {
        .type = type,
        .category = DEVICE_EVENT_CAT_MMWAVE,
        .area_id = area_id,
        .area = area,
        .person_id = person_id,
        .active = active,
        .static_count = static_count,
        .message = message,
    };
    if (device_event_emit(&ev) != ESP_OK) {
        return;
    }
    if (s_notify_cb != NULL) {
        s_notify_cb(area_id);
    }
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

static int count_fov_people(const c4004_target_info_t *targets, uint8_t n,
                            int *active_out, int *static_out)
{
    int active = 0;
    int st = 0;
    for (uint8_t i = 0; i < n; i++) {
        if (area_point_in_noise_zone((float)targets[i].x_cm, (float)targets[i].y_cm)) {
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

static void process_person_motion(const area_config_t *cfg,
                                  const c4004_target_info_t *targets,
                                  uint8_t n,
                                  TickType_t now)
{
    const TickType_t static_need = pdMS_TO_TICKS(CONFIG_EVENT_PERSON_STATIC_MS);
    const TickType_t moving_need = pdMS_TO_TICKS(CONFIG_EVENT_PERSON_MOVING_CONFIRM_MS);
    bool seen[PERSON_TRACK_MAX] = {false};

    for (uint8_t t = 0; t < n; t++) {
        if (area_point_in_noise_zone((float)targets[t].x_cm, (float)targets[t].y_cm)) {
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

        if (!targets[t].moving) {
            pt->moving_confirm_since = 0;
            if (pt->static_since == 0) {
                pt->static_since = now;
            }
            if (!pt->static_fired && (now - pt->static_since) >= static_need) {
                snprintf(msg, sizeof(msg), "人员在%s静止", area_name);
                emit_mmwave(DEVICE_EVENT_PERSON_STATIC, area_id, area_name,
                            (int)targets[t].index, -1, -1, msg);
                pt->static_fired = true;
            }
            pt->last_moving = false;
        } else {
            pt->static_since = 0;
            if (pt->static_fired) {
                if (pt->moving_confirm_since == 0) {
                    pt->moving_confirm_since = now;
                } else if ((now - pt->moving_confirm_since) >= moving_need) {
                    snprintf(msg, sizeof(msg), "人员在%s恢复运动", area_name);
                    emit_mmwave(DEVICE_EVENT_PERSON_MOVING, area_id, area_name,
                                (int)targets[t].index, -1, -1, msg);
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

static void process_range_events(int fov_people, int active, int static_count, TickType_t now)
{
    const TickType_t confirm = pdMS_TO_TICKS(CONFIG_EVENT_RANGE_CONFIRM_MS);
    const TickType_t empty_need = pdMS_TO_TICKS(CONFIG_EVENT_ROOM_EMPTY_MS);
    const bool occ = fov_people > 0;

    if (occ != s_range_pending_occ) {
        s_range_pending_occ = occ;
        s_range_stable_since = now;
    } else if (s_range_stable_since != 0 &&
               (now - s_range_stable_since) >= confirm) {
        if (occ && !s_range_occupied) {
            char msg[EVENT_MSG_LEN];
            snprintf(msg, sizeof(msg),
                     "有人进入探测范围，活动%d人，静止%d人", active, static_count);
            emit_mmwave(DEVICE_EVENT_RANGE_ENTER, DEVICE_EVENT_AREA_ID_GLOBAL, NULL,
                        -1, active, static_count, msg);
            s_range_occupied = true;
            s_room_empty_fired = false;
            s_range_empty_since = 0;
            s_post_empty_guard_until = 0;
        } else if (!occ && s_range_occupied) {
            emit_mmwave(DEVICE_EVENT_RANGE_EXIT, DEVICE_EVENT_AREA_ID_GLOBAL, NULL,
                        -1, 0, 0, "有人离开探测范围");
            s_range_occupied = false;
            s_range_empty_since = now;
            s_post_empty_guard_until =
                now + pdMS_TO_TICKS(CONFIG_MCP_RADAR_CROSSING_COOLDOWN_MS);
        }
        s_range_stable_since = 0;
    }

    if (s_range_occupied || occ) {
        s_range_empty_since = 0;
    } else {
        if (s_range_empty_since == 0) {
            s_range_empty_since = now;
        }
        if (!s_room_empty_fired && (now - s_range_empty_since) >= empty_need) {
            emit_mmwave(DEVICE_EVENT_ROOM_EMPTY, DEVICE_EVENT_AREA_ID_GLOBAL, NULL,
                        -1, -1, -1, "探测范围无人");
            s_room_empty_fired = true;
        }
    }
}

static bool emit_area_crossing(uint8_t area_id,
                               bool is_enter,
                               const char *area_name,
                               int active,
                               int static_count)
{
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

    emit_mmwave(is_enter ? DEVICE_EVENT_AREA_ENTER : DEVICE_EVENT_AREA_EXIT,
                area_id, area_name, -1, active, static_count, message);

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
            (void)emit_area_crossing(i, true, tag->name, active, static_count);
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
        if (emit_area_crossing(i, false, tag->name, active, static_count)) {
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

static void process_strategy2_crossing(uint8_t i,
                                       const area_tag_t *tag,
                                       const area_status_t *st,
                                       int active,
                                       int static_count,
                                       TickType_t now,
                                       TickType_t confirm,
                                       bool range_occupied)
{
    tag_track_t *track = &s_tag_track[i];

    if (st->enter && !track->last_enter) {
        track->enter_confirm_since = now;
        track->exit_confirm_since = 0;
    }
    if (st->exit && !track->last_exit) {
        track->exit_confirm_since = now;
        track->enter_confirm_since = 0;
    }
    track->last_enter = st->enter;
    track->last_exit = st->exit;

    if (track->enter_confirm_since != 0 &&
            (now - track->enter_confirm_since) >= confirm) {
        if (!track->session_open) {
            (void)emit_area_crossing(i, true, tag->name, active, static_count);
            track->reported_total = 1;
        }
        track->enter_confirm_since = 0;
    }

    if (track->exit_confirm_since != 0 &&
            (now - track->exit_confirm_since) >= confirm) {
        if (!should_suppress_area_exit(range_occupied, 0, now)) {
            (void)emit_area_crossing(i, false, tag->name, active, static_count);
            track->reported_total = 0;
            track->session_open = false;
        }
        track->exit_confirm_since = 0;
    }
}

static void process_area_events(const area_config_t *cfg,
                                const area_status_t *areas,
                                bool range_occupied,
                                TickType_t now)
{
    const TickType_t confirm = pdMS_TO_TICKS(CONFIG_EVENT_AREA_CONFIRM_MS);

    for (uint8_t i = 0; i < cfg->tag_count; i++) {
        const area_tag_t *tag = &cfg->tags[i];
        const area_status_t *st = &areas[i];
        int active = 0;
        int static_count = 0;
        get_tag_people_count(i, cfg, areas, &active, &static_count);

        if (tag->strategy == 2) {
            process_strategy2_crossing(i, tag, st, active, static_count, now, confirm,
                                       range_occupied);
        } else if (tag->strategy == 1) {
            process_strategy1_crossing(i, tag, cfg, areas, now, confirm, range_occupied);
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
    const int fov_people = count_fov_people(s_poll_targets, tcount, &active, &static_count);

    process_range_events(fov_people, active, static_count, now);
    process_person_motion(cfg, s_poll_targets, tcount, now);

    if (cfg != NULL && cfg->tag_count > 0 && areas != NULL) {
        process_area_events(cfg, areas, s_range_occupied, now);
    }
}

static void radar_events_watch_task(void *arg)
{
    (void)arg;
    const TickType_t interval = pdMS_TO_TICKS(CONFIG_MCP_RADAR_POLL_INTERVAL_MS);

    while (s_running) {
        const bool have_cfg = (area_config_get(&s_poll_cfg) == ESP_OK);
        if (have_cfg && s_poll_cfg.tag_count > 0 &&
            radar_area_get_status(s_poll_areas, &s_poll_area_count) == ESP_OK) {
            process_snapshot(&s_poll_cfg, s_poll_areas);
        } else {
            process_snapshot(have_cfg ? &s_poll_cfg : NULL, NULL);
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
    s_range_occupied = false;
    s_range_pending_occ = false;
    s_range_stable_since = 0;
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

    ESP_LOGI(TAG,
             "radar event watch started (poll=%dms range=%dms area=%dms empty=%dms static=%dms)",
             CONFIG_MCP_RADAR_POLL_INTERVAL_MS,
             CONFIG_EVENT_RANGE_CONFIRM_MS,
             CONFIG_EVENT_AREA_CONFIRM_MS,
             CONFIG_EVENT_ROOM_EMPTY_MS,
             CONFIG_EVENT_PERSON_STATIC_MS);
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
