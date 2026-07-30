#include "web_server.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "board_snapshot.h"
#include "c4004_sensor.h"
#include "device_event_bus.h"
#include "ota_update.h"
#include "radar_area.h"
#include "sc7a20h_sensor.h"
#include "sensor_settings.h"
#include "web_fs.h"
#include "web_log.h"
#include "captive_portal.h"
#include "wifi_manager.h"
#include "sdkconfig.h"

#if CONFIG_YAMNET_ENABLE
#include "yamnet_events.h"
#endif
#if CONFIG_ENV_SOUND_ENABLE
#include "env_sound.h"
#endif

static const char *TAG = "web_server";
static httpd_handle_t s_server = NULL;
static const char *s_index_page = "index.html";
static bool s_provision_mode = false;

/** 前端地图用米；固件/C4004 用厘米 */
#define WEB_M_PER_CM 0.01f
#define WEB_CM_PER_M 100.0f

static double round1(double v)
{
    return round(v * 10.0) / 10.0;
}

typedef struct {
    char ssid[33];
    char pass[65];
} wifi_connect_params_t;

static void wifi_connect_task(void *arg)
{
    wifi_connect_params_t *params = (wifi_connect_params_t *)arg;
    vTaskDelay(pdMS_TO_TICKS(300));
    web_server_stop();
    wifi_manager_connect_sta(params->ssid, params->pass);
    free(params);
    vTaskDelete(NULL);
}

static void wifi_reprovision_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(300));
    wifi_manager_enter_reprovision();
    vTaskDelete(NULL);
}

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str == NULL) {
        return ESP_ERR_NO_MEM;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
    free(json_str);
    return err;
}

static esp_err_t send_json_error(httpd_req_t *req, const char *http_status, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "message", message);

    httpd_resp_set_status(req, http_status);
    esp_err_t err = send_json(req, root);
    cJSON_Delete(root);
    return err;
}

static int read_request_body(httpd_req_t *req, char *buf, size_t buf_len)
{
    size_t offset = 0;
    while (offset < buf_len - 1) {
        int received = httpd_req_recv(req, buf + offset, buf_len - 1 - offset);
        if (received <= 0) {
            return received;
        }
        offset += (size_t)received;
        if (offset >= (size_t)req->content_len) {
            break;
        }
    }
    buf[offset] = '\0';
    return (int)offset;
}

static esp_err_t serve_spiffs_path(httpd_req_t *req, const char *relative_path)
{
    char filepath[128];
    snprintf(filepath, sizeof(filepath), WEB_FS_MOUNT_POINT "/%s", relative_path);
    return web_fs_send_file(req, filepath);
}

/** 去掉 ?query 与 #fragment，SPIFFS 路径不含这些后缀 */
static bool static_relpath_from_uri(const char *uri, char *out, size_t out_len)
{
    if (uri[0] != '/' || uri[1] == '\0' || out_len == 0) {
        return false;
    }

    const char *start = uri + 1;
    size_t i = 0;
    while (start[i] != '\0' && start[i] != '?' && start[i] != '#' && i < out_len - 1) {
        out[i] = start[i];
        i++;
    }
    out[i] = '\0';
    return i > 0;
}

static esp_err_t static_file_handler(httpd_req_t *req)
{
    const char *uri = req->uri;

    if (strncmp(uri, "/api/", 5) == 0) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    if (strncmp(uri, "/mcp", 4) == 0) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    if (s_provision_mode && captive_portal_is_probe_uri(uri)) {
        return captive_portal_redirect(req);
    }

    if (strcmp(uri, "/") == 0) {
        return serve_spiffs_path(req, s_index_page);
    }

    char rel_path[128];
    if (!static_relpath_from_uri(uri, rel_path, sizeof(rel_path))) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }
    return serve_spiffs_path(req, rel_path);
}

static cJSON *coords_cm_to_m_json(const float coords_cm[4][2])
{
    cJSON *arr = cJSON_CreateArray();
    for (int p = 0; p < 4; p++) {
        cJSON *pt = cJSON_CreateArray();
        cJSON_AddItemToArray(pt, cJSON_CreateNumber(round1(coords_cm[p][0] * WEB_M_PER_CM)));
        cJSON_AddItemToArray(pt, cJSON_CreateNumber(round1(coords_cm[p][1] * WEB_M_PER_CM)));
        cJSON_AddItemToArray(arr, pt);
    }
    return arr;
}

static bool parse_tag_coords_m(const cJSON *coords, area_tag_t *tag)
{
    if (!cJSON_IsArray(coords) || tag == NULL) {
        return false;
    }
    for (int p = 0; p < 4; p++) {
        const cJSON *pt = cJSON_GetArrayItem(coords, p);
        if (pt == NULL || !cJSON_IsArray(pt) || cJSON_GetArraySize(pt) < 2) {
            tag->coords[p][0] = 0.0f;
            tag->coords[p][1] = 0.0f;
            continue;
        }
        const cJSON *jx = cJSON_GetArrayItem(pt, 0);
        const cJSON *jy = cJSON_GetArrayItem(pt, 1);
        float xm = cJSON_IsNumber(jx) ? (float)jx->valuedouble : 0.0f;
        float ym = cJSON_IsNumber(jy) ? (float)jy->valuedouble : 0.0f;
        tag->coords[p][0] = xm * WEB_CM_PER_M;
        tag->coords[p][1] = ym * WEB_CM_PER_M;
    }
    return true;
}

static bool parse_quad_coords_m(const cJSON *coords, float out_cm[4][2])
{
    if (!cJSON_IsArray(coords) || out_cm == NULL) {
        return false;
    }
    for (int p = 0; p < 4; p++) {
        const cJSON *pt = cJSON_GetArrayItem(coords, p);
        if (pt == NULL || !cJSON_IsArray(pt) || cJSON_GetArraySize(pt) < 2) {
            return false;
        }
        const cJSON *jx = cJSON_GetArrayItem(pt, 0);
        const cJSON *jy = cJSON_GetArrayItem(pt, 1);
        if (!cJSON_IsNumber(jx) || !cJSON_IsNumber(jy)) {
            return false;
        }
        out_cm[p][0] = (float)jx->valuedouble * WEB_CM_PER_M;
        out_cm[p][1] = (float)jy->valuedouble * WEB_CM_PER_M;
    }
    return true;
}

static void fill_tag_from_json(const cJSON *root, area_tag_t *tag)
{
    memset(tag, 0, sizeof(*tag));
    const cJSON *strategy = cJSON_GetObjectItem(root, "strategy");
    const cJSON *name = cJSON_GetObjectItem(root, "name");
    const cJSON *setting = cJSON_GetObjectItem(root, "setting");
    const cJSON *coords = cJSON_GetObjectItem(root, "coords");
    if (cJSON_IsNumber(strategy)) {
        tag->strategy = (uint8_t)strategy->valueint;
    }
    if (cJSON_IsString(name) && name->valuestring != NULL) {
        strncpy(tag->name, name->valuestring, sizeof(tag->name) - 1);
    }
    if (cJSON_IsString(setting) && setting->valuestring != NULL) {
        strncpy(tag->setting, setting->valuestring, sizeof(tag->setting) - 1);
    }
    parse_tag_coords_m(coords, tag);
}

static esp_err_t api_data_handler(httpd_req_t *req)
{
    sensor_snapshot_t snapshot = {0};
    board_snapshot_get(&snapshot);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "env_ready", snapshot.env_ready);

    if (snapshot.env_ready) {
        cJSON_AddNumberToObject(root, "temp", round1(snapshot.temperature));
        cJSON_AddNumberToObject(root, "humidity", round1(snapshot.humidity));
        cJSON_AddNumberToObject(root, "co2", snapshot.co2);
    } else {
        cJSON_AddNullToObject(root, "temp");
        cJSON_AddNullToObject(root, "humidity");
        cJSON_AddNullToObject(root, "co2");
    }

    cJSON_AddBoolToObject(root, "lux_ready", snapshot.lux_ready);
    if (snapshot.lux_ready) {
        const int lux_i = (int)lround(snapshot.lux);
        cJSON_AddNumberToObject(root, "lux", lux_i);
        cJSON_AddNumberToObject(root, "light", lux_i);
    } else {
        cJSON_AddNullToObject(root, "lux");
        cJSON_AddNullToObject(root, "light");
    }

#if CONFIG_YAMNET_ENABLE
    {
        bool added = false;
        char *sound_json = NULL;
        if (device_event_read_latest_by_category(DEVICE_EVENT_CAT_SOUND, &sound_json) == ESP_OK &&
            sound_json != NULL) {
            cJSON *src = cJSON_Parse(sound_json);
            free(sound_json);
            if (src != NULL) {
                const cJSON *type = cJSON_GetObjectItem(src, "type");
                const cJSON *msg = cJSON_GetObjectItem(src, "message");
                const cJSON *ts = cJSON_GetObjectItem(src, "timestamp");
                const cJSON *pl = cJSON_GetObjectItem(src, "payload");
                const cJSON *id = cJSON_IsObject(pl) ? cJSON_GetObjectItem(pl, "id") : NULL;
                cJSON *sound_event = cJSON_AddObjectToObject(root, "sound_event");
                if (sound_event != NULL) {
                    cJSON_AddStringToObject(sound_event, "id",
                                           cJSON_IsString(id) ? id->valuestring :
                                           (cJSON_IsString(type) ? type->valuestring : ""));
                    cJSON_AddStringToObject(sound_event, "type",
                                           cJSON_IsString(msg) ? msg->valuestring :
                                           (cJSON_IsString(type) ? type->valuestring : ""));
                    cJSON_AddStringToObject(sound_event, "time",
                                           cJSON_IsString(ts) ? ts->valuestring : "—");
                    added = true;
                }
                cJSON_Delete(src);
            }
        }
        if (!added) {
            char evt_type[24] = {0};
            char evt_label[32] = {0};
            uint32_t evt_age_ms = 0;
            if (yamnet_events_get_last(evt_type, sizeof(evt_type), evt_label, sizeof(evt_label),
                                       &evt_age_ms) &&
                evt_age_ms <= 24u * 3600u * 1000u) {
                cJSON *sound_event = cJSON_AddObjectToObject(root, "sound_event");
                cJSON_AddStringToObject(sound_event, "id", evt_type);
                cJSON_AddStringToObject(sound_event, "type", evt_label[0] ? evt_label : evt_type);
                if (evt_age_ms < 5000) {
                    cJSON_AddStringToObject(sound_event, "time", "刚刚");
                } else if (evt_age_ms < 60000) {
                    char buf[24];
                    snprintf(buf, sizeof(buf), "%u 秒前", (unsigned)(evt_age_ms / 1000));
                    cJSON_AddStringToObject(sound_event, "time", buf);
                } else if (evt_age_ms < 3600000) {
                    char buf[24];
                    snprintf(buf, sizeof(buf), "%u 分钟前", (unsigned)(evt_age_ms / 60000));
                    cJSON_AddStringToObject(sound_event, "time", buf);
                } else {
                    char buf[24];
                    snprintf(buf, sizeof(buf), "%u 小时前", (unsigned)(evt_age_ms / 3600000));
                    cJSON_AddStringToObject(sound_event, "time", buf);
                }
            } else {
                cJSON_AddNullToObject(root, "sound_event");
            }
        }
    }
#else
    cJSON_AddNullToObject(root, "sound_event");
#endif

#if CONFIG_ENV_SOUND_ENABLE
    float sound_db = 0.0f;
    if (sensor_settings_sound_enabled() && env_sound_get_db(&sound_db)) {
        sound_db += sensor_settings_sound_offset_db();
        if (sound_db < 0.0f) {
            sound_db = 0.0f;
        } else if (sound_db > 140.0f) {
            sound_db = 140.0f;
        }
        cJSON_AddNumberToObject(root, "sound", (int)lround(sound_db));
    } else {
        cJSON_AddNullToObject(root, "sound");
    }
#else
    cJSON_AddNullToObject(root, "sound");
#endif

    cJSON *areas = cJSON_AddArrayToObject(root, "areas");
    int active_sum = 0;
    int static_sum = 0;
    for (uint8_t i = 0; i < snapshot.area_count; i++) {
        const area_status_t *st = &snapshot.areas[i];
        cJSON *area = cJSON_CreateObject();
        cJSON_AddStringToObject(area, "name", st->name);
        cJSON_AddNumberToObject(area, "strategy", st->strategy);

        switch (st->strategy) {
        case 2:
            cJSON_AddBoolToObject(area, "enter", st->enter);
            cJSON_AddBoolToObject(area, "exit", st->exit);
            break;
        case 3:
            cJSON_AddBoolToObject(area, "approaching", st->approaching);
            cJSON_AddBoolToObject(area, "leaving", st->leaving);
            break;
        case 4:
            /* C4004 Noise：干扰抑制区，不计入人数 */
            cJSON_AddStringToObject(area, "role", "noise");
            break;
        case 5:
            /* C4004 None：空标签占位 */
            cJSON_AddStringToObject(area, "role", "none");
            break;
        default:
            cJSON_AddNumberToObject(area, "active", st->active);
            cJSON_AddNumberToObject(area, "static", st->static_count);
            active_sum += st->active;
            static_sum += st->static_count;
            break;
        }
        cJSON_AddItemToArray(areas, area);
    }

    cJSON *mmwave = cJSON_AddObjectToObject(root, "mmwave");
    const char *install_status = "unknown";
    sc7a20h_tilt_t tilt = {0};
    if (sc7a20h_sensor_get_tilt(&tilt) == ESP_OK) {
        install_status = tilt.upright ? "normal" : "abnormal";
    }
    cJSON_AddStringToObject(mmwave, "install_status", install_status);

    const char *detect_status = "offline";
    if (c4004_sensor_is_ready()) {
        detect_status = "detecting";
    } else {
        detect_status = "idle";
    }
    cJSON_AddStringToObject(mmwave, "detect_status", detect_status);

    int16_t fov_xmin = 0, fov_xmax = 0, fov_ymin = 0, fov_ymax = 0;
    c4004_sensor_get_fov_cm(&fov_xmin, &fov_xmax, &fov_ymin, &fov_ymax);
    cJSON *fov = cJSON_AddObjectToObject(mmwave, "fov");
    cJSON_AddNumberToObject(fov, "x_min", round1(fov_xmin * WEB_M_PER_CM));
    cJSON_AddNumberToObject(fov, "x_max", round1(fov_xmax * WEB_M_PER_CM));
    cJSON_AddNumberToObject(fov, "y_min", round1(fov_ymin * WEB_M_PER_CM));
    cJSON_AddNumberToObject(fov, "y_max", round1(fov_ymax * WEB_M_PER_CM));

    cJSON *targets = cJSON_AddArrayToObject(mmwave, "targets");
    int traj_active = 0;
    int traj_static = 0;
    float tx = 0.0f, ty = 0.0f;
    uint8_t tcount = 0;
    int8_t picked = -1;
    if (c4004_sensor_is_ready() &&
        c4004_sensor_query_target(&tx, &ty, &tcount, &picked) == ESP_OK && tcount > 0) {
        c4004_target_info_t infos[8];
        uint8_t n = 0;
        if (c4004_sensor_get_targets(infos, 8, &n) == ESP_OK) {
            for (uint8_t i = 0; i < n; i++) {
                const bool in_noise = area_point_in_noise_zone((float)infos[i].x_cm,
                                                              (float)infos[i].y_cm);
                if (!in_noise) {
                    if (infos[i].moving) {
                        traj_active++;
                    } else {
                        traj_static++;
                    }
                }
                cJSON *t = cJSON_CreateObject();
                cJSON_AddNumberToObject(t, "id", infos[i].index);
                cJSON_AddNumberToObject(t, "x", round1(infos[i].x_cm * WEB_M_PER_CM));
                cJSON_AddNumberToObject(t, "y", round1(infos[i].y_cm * WEB_M_PER_CM));
                cJSON_AddNumberToObject(t, "speed", infos[i].speed_cm_s);
                cJSON_AddNumberToObject(t, "kinesia", infos[i].kinesia);
                cJSON_AddBoolToObject(t, "moving", infos[i].moving);
                cJSON_AddBoolToObject(t, "picked", (int)i == (int)picked);
                cJSON_AddBoolToObject(t, "noise_suppressed", in_noise);
                cJSON_AddItemToArray(targets, t);
            }
        }
    }

    /* 实时监测人数优先用轨迹；无目标时回退区域 Tag 汇总 */
    if ((traj_active + traj_static) > 0) {
        cJSON_AddNumberToObject(mmwave, "active_count", traj_active);
        cJSON_AddNumberToObject(mmwave, "static_count", traj_static);
    } else {
        cJSON_AddNumberToObject(mmwave, "active_count", active_sum);
        cJSON_AddNumberToObject(mmwave, "static_count", static_sum);
    }
    cJSON_AddNumberToObject(mmwave, "target_count", traj_active + traj_static);
    {
        char *le_json = NULL;
        if (device_event_read_latest_mmwave(&le_json) == ESP_OK && le_json != NULL) {
            cJSON *le = cJSON_Parse(le_json);
            free(le_json);
            if (le != NULL) {
                cJSON_AddItemToObject(mmwave, "last_event", le);
            } else {
                cJSON_AddNullToObject(mmwave, "last_event");
            }
        } else {
            cJSON_AddNullToObject(mmwave, "last_event");
        }
    }

    esp_err_t err = send_json(req, root);
    cJSON_Delete(root);
    return err;
}

static esp_err_t api_events_handler(httpd_req_t *req)
{
    char query[96] = {0};
    char area_buf[16] = {0};
    char cat_buf[16] = {0};
    bool filter_area = false;
    uint8_t area_id = 0;
    const char *category = NULL;

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        if (httpd_query_key_value(query, "area", area_buf, sizeof(area_buf)) == ESP_OK) {
            int v = atoi(area_buf);
            if (v >= 0 && v < AREA_TAG_MAX) {
                area_id = (uint8_t)v;
                filter_area = true;
            }
        }
        if (httpd_query_key_value(query, "category", cat_buf, sizeof(cat_buf)) == ESP_OK) {
            if (strcmp(cat_buf, "mmwave") == 0 || strcmp(cat_buf, "env") == 0 ||
                strcmp(cat_buf, "sound") == 0 || strcmp(cat_buf, "heartbeat") == 0) {
                category = cat_buf;
            }
        }
    }

    (void)device_event_bus_init();

    char *payload = NULL;
    esp_err_t err;
    if (filter_area) {
        err = device_event_read_area(area_id, &payload);
        if (err == ESP_ERR_NOT_FOUND) {
            cJSON *root = cJSON_CreateObject();
            if (root == NULL) {
                return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
            }
            cJSON_AddBoolToObject(root, "ready", false);
            cJSON_AddNumberToObject(root, "area_id", area_id);
            err = send_json(req, root);
            cJSON_Delete(root);
            return err;
        }
    } else {
        err = device_event_read_feed_filtered(category, &payload);
    }

    if (err != ESP_OK || payload == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "events read failed");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    err = httpd_resp_send(req, payload, HTTPD_RESP_USE_STRLEN);
    free(payload);
    return err;
}

static cJSON *area_tags_to_json(const area_config_t *cfg)
{
    cJSON *tags = cJSON_CreateArray();
    for (uint8_t i = 0; i < cfg->tag_count; i++) {
        const area_tag_t *tag = &cfg->tags[i];
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "id", i);
        cJSON_AddNumberToObject(item, "strategy", tag->strategy);
        cJSON_AddStringToObject(item, "name", tag->name);
        if (tag->setting[0] != '\0') {
            cJSON_AddStringToObject(item, "setting", tag->setting);
        } else {
            cJSON_AddNullToObject(item, "setting");
        }
        cJSON_AddItemToObject(item, "coords", coords_cm_to_m_json(tag->coords));
        cJSON_AddItemToArray(tags, item);
    }
    return tags;
}

static esp_err_t api_config_get_handler(httpd_req_t *req)
{
    area_config_t cfg = {0};
    area_config_get(&cfg);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "tags", area_tags_to_json(&cfg));

    if (cfg.has_detection_range) {
        cJSON *range = cJSON_AddObjectToObject(root, "detection_range");
        cJSON_AddItemToObject(range, "coords", coords_cm_to_m_json(cfg.detection_range));
    } else {
        cJSON_AddNullToObject(root, "detection_range");
    }

    esp_err_t err = send_json(req, root);
    cJSON_Delete(root);
    return err;
}

static esp_err_t api_config_post_handler(httpd_req_t *req)
{
    char body[1536] = {0};
    if (read_request_body(req, body, sizeof(body)) <= 0) {
        return send_json_error(req, "400 Bad Request", "invalid body");
    }

    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        return send_json_error(req, "400 Bad Request", "invalid json");
    }

    const cJSON *action = cJSON_GetObjectItem(root, "action");
    esp_err_t save_err = ESP_OK;
    const char *message = "操作成功";
    int new_id = -1;

    if (cJSON_IsString(action) && strcmp(action->valuestring, "set_range") == 0) {
        const cJSON *coords = cJSON_GetObjectItem(root, "coords");
        float range_cm[4][2] = {0};
        if (!parse_quad_coords_m(coords, range_cm)) {
            cJSON_Delete(root);
            return send_json_error(req, "400 Bad Request", "invalid detection range coords");
        }
        save_err = area_config_set_detection_range(range_cm);
        message = "检测范围已保存";
    } else if (cJSON_IsString(action) && strcmp(action->valuestring, "add_tag") == 0) {
        area_tag_t tag = {0};
        fill_tag_from_json(root, &tag);
        if (tag.name[0] == '\0') {
            cJSON_Delete(root);
            return send_json_error(req, "400 Bad Request", "missing tag fields");
        }
        if (!area_coords_valid(&tag)) {
            cJSON_Delete(root);
            return send_json_error(req, "400 Bad Request",
                                   "请先获取 4 个有效坐标点（区域宽高至少 10cm）");
        }
        uint8_t id = 0;
        save_err = area_config_add_tag(&tag, &id);
        new_id = id;
        message = "区域标签已添加";
    } else if (cJSON_IsString(action) && strcmp(action->valuestring, "update_tag") == 0) {
        const cJSON *id = cJSON_GetObjectItem(root, "id");
        if (!cJSON_IsNumber(id)) {
            cJSON_Delete(root);
            return send_json_error(req, "400 Bad Request", "missing tag id");
        }
        area_tag_t tag = {0};
        fill_tag_from_json(root, &tag);
        if (tag.name[0] == '\0' || !area_coords_valid(&tag)) {
            cJSON_Delete(root);
            return send_json_error(req, "400 Bad Request", "invalid tag fields");
        }
        save_err = area_config_update_tag((uint8_t)id->valueint, &tag);
        message = "区域标签已更新";
    } else if (cJSON_IsString(action) && strcmp(action->valuestring, "delete_tag") == 0) {
        const cJSON *id = cJSON_GetObjectItem(root, "id");
        if (!cJSON_IsNumber(id)) {
            cJSON_Delete(root);
            return send_json_error(req, "400 Bad Request", "missing tag id");
        }
        save_err = area_config_remove_tag((uint8_t)id->valueint);
        message = "区域标签已删除";
    } else {
        cJSON_Delete(root);
        return send_json_error(req, "400 Bad Request", "unknown action");
    }

    cJSON_Delete(root);

    if (save_err != ESP_OK) {
        return send_json_error(req, "500 Internal Server Error", "save failed");
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddStringToObject(resp, "message", message);
    if (new_id >= 0) {
        cJSON_AddNumberToObject(resp, "id", new_id);
    }
    esp_err_t err = send_json(req, resp);
    cJSON_Delete(resp);
    return err;
}

/** 从 C4004 读取当前目标坐标（返回米，供 Web 采集区域角点） */
static esp_err_t api_coord_handler(httpd_req_t *req)
{
    const char *point_str = NULL;
    char query[32];
    char value[8];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        if (httpd_query_key_value(query, "point", value, sizeof(value)) == ESP_OK) {
            point_str = value;
        }
    }

    int point = point_str ? atoi(point_str) : 0;
    if (point < 1 || point > 4) {
        return send_json_error(req, "400 Bad Request", "point must be 1-4");
    }

    float x = 0.0f;
    float y = 0.0f;
    uint8_t target_count = 0;
    int8_t picked_index = -1;
    esp_err_t err = radar_area_fetch_coord(&x, &y, &target_count, &picked_index);

    cJSON *root = cJSON_CreateObject();
    if (err == ESP_OK) {
        cJSON_AddBoolToObject(root, "ok", true);
        cJSON_AddNumberToObject(root, "point", point);
        cJSON_AddNumberToObject(root, "x", round1(x * WEB_M_PER_CM));
        cJSON_AddNumberToObject(root, "y", round1(y * WEB_M_PER_CM));
        cJSON_AddNumberToObject(root, "targets", target_count);
        if (picked_index >= 0) {
            cJSON_AddNumberToObject(root, "picked", picked_index);
        }
        if (target_count > 1) {
            cJSON_AddStringToObject(root, "message",
                                    "检测到多个目标，已选用运动目标；标定时请仅一人在角点");
        } else {
            cJSON_AddStringToObject(root, "message", "已从 C4004 读取目标坐标");
        }
        ESP_LOGI(TAG, "coord point=%d x=%.2fm y=%.2fm targets=%u picked=%d",
                 point, x * WEB_M_PER_CM, y * WEB_M_PER_CM, target_count, (int)picked_index);
    } else if (err == ESP_ERR_NOT_FOUND) {
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON_AddNumberToObject(root, "point", point);
        cJSON_AddStringToObject(root, "message", "未检测到目标，请站在角点位置后重试");
        ESP_LOGW(TAG, "coord point=%d: no target", point);
    } else if (err == ESP_ERR_INVALID_STATE) {
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON_AddNumberToObject(root, "point", point);
        cJSON_AddStringToObject(root, "message", "C4004 未就绪，请检查接线与串口日志");
    } else {
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON_AddNumberToObject(root, "point", point);
        cJSON_AddStringToObject(root, "message", "读取坐标超时，请重试");
    }

    esp_err_t send_err = send_json(req, root);
    cJSON_Delete(root);
    return send_err;
}

static esp_err_t api_wifi_get_handler(httpd_req_t *req)
{
    wifi_status_t st = {0};
    wifi_manager_get_status(&st);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "configured", st.configured);
    cJSON_AddBoolToObject(root, "connected", st.connected);
    cJSON_AddBoolToObject(root, "connecting", st.connecting);
    cJSON_AddBoolToObject(root, "ap_mode", st.ap_mode);
    cJSON_AddStringToObject(root, "ssid", st.ssid);
    cJSON_AddStringToObject(root, "password", st.password);
    cJSON_AddStringToObject(root, "ip", st.ip);

    esp_err_t err = send_json(req, root);
    cJSON_Delete(root);
    return err;
}

static void wifi_disconnect_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(300));
    web_server_stop();
    wifi_manager_disconnect();
    vTaskDelete(NULL);
}

static esp_err_t api_wifi_post_handler(httpd_req_t *req)
{
    char body[256] = {0};
    if (read_request_body(req, body, sizeof(body)) <= 0) {
        return send_json_error(req, "400 Bad Request", "invalid body");
    }

    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        return send_json_error(req, "400 Bad Request", "invalid json");
    }

    const cJSON *action = cJSON_GetObjectItem(root, "action");
    if (cJSON_IsString(action) && strcmp(action->valuestring, "reprovision") == 0) {
        cJSON_Delete(root);

        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", true);
        cJSON_AddStringToObject(resp, "message", "正在开启配网热点 MCP-Sensors-Setup…");
        esp_err_t err = send_json(req, resp);
        cJSON_Delete(resp);
        if (err != ESP_OK) {
            return err;
        }

        if (xTaskCreate(wifi_reprovision_task, "wifi_reprov", 4096, NULL, 5, NULL) != pdPASS) {
            return ESP_ERR_NO_MEM;
        }
        return ESP_OK;
    }

    if (cJSON_IsString(action) && strcmp(action->valuestring, "disconnect") == 0) {
        cJSON_Delete(root);
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", true);
        cJSON_AddStringToObject(resp, "message", "正在断开并返回配网热点…");
        esp_err_t err = send_json(req, resp);
        cJSON_Delete(resp);
        if (err != ESP_OK) {
            return err;
        }
        if (xTaskCreate(wifi_disconnect_task, "wifi_disc", 4096, NULL, 5, NULL) != pdPASS) {
            return ESP_ERR_NO_MEM;
        }
        return ESP_OK;
    }

    const cJSON *ssid_item = cJSON_GetObjectItem(root, "ssid");
    const cJSON *pass_item = cJSON_GetObjectItem(root, "password");
    if (!cJSON_IsString(ssid_item) || ssid_item->valuestring[0] == '\0') {
        cJSON_Delete(root);
        return send_json_error(req, "400 Bad Request", "missing ssid");
    }

    char ssid_buf[33] = {0};
    char pass_buf[65] = {0};
    strncpy(ssid_buf, ssid_item->valuestring, sizeof(ssid_buf) - 1);
    if (cJSON_IsString(pass_item)) {
        strncpy(pass_buf, pass_item->valuestring, sizeof(pass_buf) - 1);
    }
    ESP_LOGI(TAG, "save WiFi via API: ssid=%s", ssid_buf);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddStringToObject(resp, "message", "正在连接 WiFi...");
    esp_err_t err = send_json(req, resp);
    cJSON_Delete(resp);
    cJSON_Delete(root);

    if (err != ESP_OK) {
        return err;
    }

    wifi_connect_params_t *params = calloc(1, sizeof(*params));
    if (params == NULL) {
        return ESP_ERR_NO_MEM;
    }
    strncpy(params->ssid, ssid_buf, sizeof(params->ssid) - 1);
    strncpy(params->pass, pass_buf, sizeof(params->pass) - 1);

    if (xTaskCreate(wifi_connect_task, "wifi_connect", 4096, params, 5, NULL) != pdPASS) {
        free(params);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t api_wifi_scan_handler(httpd_req_t *req)
{
    wifi_scan_ap_t aps[24];
    uint16_t count = 0;
    esp_err_t scan_err = wifi_manager_scan(aps, 24, &count);

    cJSON *root = cJSON_CreateObject();
    if (scan_err != ESP_OK) {
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON_AddStringToObject(root, "message", "WiFi 扫描失败");
        cJSON_AddArrayToObject(root, "networks");
        httpd_resp_set_status(req, "500 Internal Server Error");
    } else {
        cJSON_AddBoolToObject(root, "ok", true);
        cJSON *networks = cJSON_AddArrayToObject(root, "networks");
        for (uint16_t i = 0; i < count; i++) {
            if (aps[i].ssid[0] == '\0') {
                continue;
            }
            cJSON *item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "ssid", aps[i].ssid);
            cJSON_AddNumberToObject(item, "rssi", aps[i].rssi);
            cJSON_AddBoolToObject(item, "secure", aps[i].secure);
            cJSON_AddStringToObject(item, "auth", aps[i].auth);
            cJSON_AddItemToArray(networks, item);
        }
    }

    esp_err_t err = send_json(req, root);
    cJSON_Delete(root);
    return err;
}

static esp_err_t api_sensors_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON *sensors = sensor_settings_to_json();
    if (sensors == NULL) {
        cJSON_Delete(root);
        return send_json_error(req, "500 Internal Server Error", "sensors unavailable");
    }
    cJSON_AddItemToObject(root, "sensors", sensors);
    esp_err_t err = send_json(req, root);
    cJSON_Delete(root);
    return err;
}

static esp_err_t api_sensors_post_handler(httpd_req_t *req)
{
    char body[3072] = {0};
    if (read_request_body(req, body, sizeof(body)) <= 0) {
        return send_json_error(req, "400 Bad Request", "invalid body");
    }

    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        return send_json_error(req, "400 Bad Request", "invalid json");
    }

    const cJSON *sensors = cJSON_GetObjectItem(root, "sensors");
    esp_err_t save_err = sensor_settings_from_json(sensors);
    cJSON_Delete(root);
    if (save_err != ESP_OK) {
        return send_json_error(req, "400 Bad Request", "invalid sensors payload");
    }

    /* 阈值页不再改安装高度，无需每次保存都同步 C4004 */
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddStringToObject(resp, "message", "阈值已保存");
    esp_err_t err = send_json(req, resp);
    cJSON_Delete(resp);
    return err;
}

#define OTA_UPLOAD_CHUNK_SIZE 4096

static void ota_reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

static esp_err_t api_ota_get_handler(httpd_req_t *req)
{
    ota_info_t info = {0};
    ota_update_get_info(&info);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "version", info.version);
    cJSON_AddStringToObject(root, "running", info.running_label);
    cJSON_AddBoolToObject(root, "pending_verify", info.pending_verify);
    cJSON_AddNumberToObject(root, "storage_size", info.storage_size);

    esp_err_t err = send_json(req, root);
    cJSON_Delete(root);
    return err;
}

/** POST /api/ota?target=app|ui&reboot=0|1  原始二进制上传 */
static esp_err_t api_ota_post_handler(httpd_req_t *req)
{
    if (req->content_len <= 0) {
        return send_json_error(req, "400 Bad Request", "请选择升级文件");
    }

    char query[64] = {0};
    char target[16] = "app";
    bool do_reboot = true;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char buf[16];
        if (httpd_query_key_value(query, "target", target, sizeof(target)) != ESP_OK) {
            strncpy(target, "app", sizeof(target) - 1);
        }
        if (httpd_query_key_value(query, "reboot", buf, sizeof(buf)) == ESP_OK) {
            do_reboot = !(buf[0] == '0' && buf[1] == '\0');
        }
    }

    const bool is_ui = (strcmp(target, "ui") == 0 || strcmp(target, "storage") == 0);
    if (!is_ui && strcmp(target, "app") != 0) {
        return send_json_error(req, "400 Bad Request", "target 应为 app 或 ui");
    }

    esp_err_t err;
    if (is_ui) {
        err = ota_update_spiffs_begin((size_t)req->content_len);
        if (err != ESP_OK) {
            const char *msg = "网页分区升级初始化失败";
            if (err == ESP_ERR_NOT_FOUND) {
                msg = "未找到 storage 分区";
            } else if (err == ESP_ERR_INVALID_SIZE) {
                msg = "网页镜像过大或为空";
            }
            return send_json_error(req, "500 Internal Server Error", msg);
        }
    } else {
        err = ota_update_begin();
        if (err != ESP_OK) {
            return send_json_error(req, "500 Internal Server Error",
                                   err == ESP_ERR_NOT_FOUND ? "没有可用的 OTA 分区" : "OTA 初始化失败");
        }
    }

    uint8_t *buf = malloc(OTA_UPLOAD_CHUNK_SIZE);
    if (buf == NULL) {
        if (is_ui) {
            ota_update_spiffs_abort();
        } else {
            ota_update_abort();
        }
        return send_json_error(req, "500 Internal Server Error", "内存不足");
    }

    int remaining = (int)req->content_len;
    bool write_failed = false;
    while (remaining > 0) {
        int to_read = remaining < OTA_UPLOAD_CHUNK_SIZE ? remaining : OTA_UPLOAD_CHUNK_SIZE;
        int received = httpd_req_recv(req, (char *)buf, to_read);
        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (received <= 0) {
            write_failed = true;
            break;
        }
        err = is_ui ? ota_update_spiffs_write(buf, (size_t)received)
                    : ota_update_write(buf, (size_t)received);
        if (err != ESP_OK) {
            write_failed = true;
            break;
        }
        remaining -= received;
    }
    free(buf);

    if (write_failed) {
        if (is_ui) {
            ota_update_spiffs_abort();
        } else {
            ota_update_abort();
        }
        ESP_LOGE(TAG, "OTA upload interrupted (%s, %d bytes remaining)", target, remaining);
        return send_json_error(req, "500 Internal Server Error", "上传中断，请重试");
    }

    err = is_ui ? ota_update_spiffs_end() : ota_update_end();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA end failed (%s): %s", target, esp_err_to_name(err));
        return send_json_error(req, "400 Bad Request",
                               is_ui ? "网页镜像写入失败，请确认是 build/storage.bin"
                                     : "固件校验失败，请确认文件正确且完整");
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddStringToObject(resp, "target", is_ui ? "ui" : "app");
    if (is_ui && !do_reboot) {
        cJSON_AddStringToObject(resp, "message", "网页已更新，可继续上传固件或强制刷新页面");
    } else if (is_ui) {
        cJSON_AddStringToObject(resp, "message", "网页已更新，设备将在数秒后重启…");
    } else {
        cJSON_AddStringToObject(resp, "message", "固件升级成功，设备将在数秒后重启…");
    }
    esp_err_t send_err = send_json(req, resp);
    cJSON_Delete(resp);

    if (do_reboot) {
        ESP_LOGI(TAG, "OTA success (%s), rebooting shortly", target);
        if (xTaskCreate(ota_reboot_task, "ota_reboot", 2048, NULL, 5, NULL) != pdPASS) {
            esp_restart();
        }
    } else {
        ESP_LOGI(TAG, "OTA success (%s), reboot skipped", target);
    }
    return send_err;
}

static esp_err_t api_logs_get_handler(httpd_req_t *req)
{
    char query[128] = {0};
    uint32_t since = 0;
    int limit = 100;
    char tag[32] = {0};

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char buf[32];
        if (httpd_query_key_value(query, "since", buf, sizeof(buf)) == ESP_OK) {
            since = (uint32_t)strtoul(buf, NULL, 10);
        }
        if (httpd_query_key_value(query, "limit", buf, sizeof(buf)) == ESP_OK) {
            limit = atoi(buf);
        }
        if (httpd_query_key_value(query, "tag", tag, sizeof(tag)) == ESP_OK) {
            /* keep tag */
        }
    }

    cJSON *root = web_log_to_json(since, tag[0] ? tag : NULL, limit);
    if (root == NULL) {
        return send_json_error(req, "500 Internal Server Error", "logs unavailable");
    }
    cJSON_AddBoolToObject(root, "ok", true);
    esp_err_t err = send_json(req, root);
    cJSON_Delete(root);
    return err;
}

static esp_err_t api_logs_post_handler(httpd_req_t *req)
{
    char body[128] = {0};
    int n = 0;
    if (req->content_len > 0) {
        n = read_request_body(req, body, sizeof(body));
        if (n < 0) {
            return send_json_error(req, "400 Bad Request", "invalid body");
        }
    }

    cJSON *root = cJSON_Parse(body[0] ? body : "{\"action\":\"clear\"}");
    if (root == NULL) {
        return send_json_error(req, "400 Bad Request", "invalid json");
    }
    const cJSON *action = cJSON_GetObjectItem(root, "action");
    if (!cJSON_IsString(action) || strcmp(action->valuestring, "clear") != 0) {
        cJSON_Delete(root);
        return send_json_error(req, "400 Bad Request", "unsupported action");
    }
    web_log_clear();
    cJSON_Delete(root);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddStringToObject(resp, "message", "日志已清空");
    esp_err_t err = send_json(req, resp);
    cJSON_Delete(resp);
    return err;
}

static esp_err_t captive_redirect_handler(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    if (s_provision_mode) {
        return captive_portal_redirect(req);
    }
    httpd_resp_send_404(req);
    return ESP_FAIL;
}

static void register_api_handlers(void)
{
    httpd_uri_t data = {.uri = "/api/data", .method = HTTP_GET, .handler = api_data_handler};
    httpd_uri_t events = {.uri = "/api/events", .method = HTTP_GET, .handler = api_events_handler};
    httpd_uri_t cfg_get = {.uri = "/api/config", .method = HTTP_GET, .handler = api_config_get_handler};
    httpd_uri_t cfg_post = {.uri = "/api/config", .method = HTTP_POST, .handler = api_config_post_handler};
    httpd_uri_t coord = {.uri = "/api/coord", .method = HTTP_GET, .handler = api_coord_handler};
    httpd_uri_t wifi_get = {.uri = "/api/wifi", .method = HTTP_GET, .handler = api_wifi_get_handler};
    httpd_uri_t wifi = {.uri = "/api/wifi", .method = HTTP_POST, .handler = api_wifi_post_handler};
    httpd_uri_t wifi_scan = {.uri = "/api/wifi/scan", .method = HTTP_GET, .handler = api_wifi_scan_handler};
    httpd_uri_t sensors_get = {.uri = "/api/sensors", .method = HTTP_GET, .handler = api_sensors_get_handler};
    httpd_uri_t sensors_post = {.uri = "/api/sensors", .method = HTTP_POST, .handler = api_sensors_post_handler};
    httpd_uri_t ota_get = {.uri = "/api/ota", .method = HTTP_GET, .handler = api_ota_get_handler};
    httpd_uri_t ota_post = {.uri = "/api/ota", .method = HTTP_POST, .handler = api_ota_post_handler};
    httpd_uri_t logs_get = {.uri = "/api/logs", .method = HTTP_GET, .handler = api_logs_get_handler};
    httpd_uri_t logs_post = {.uri = "/api/logs", .method = HTTP_POST, .handler = api_logs_post_handler};

    httpd_register_uri_handler(s_server, &data);
    httpd_register_uri_handler(s_server, &events);
    httpd_register_uri_handler(s_server, &cfg_get);
    httpd_register_uri_handler(s_server, &cfg_post);
    httpd_register_uri_handler(s_server, &coord);
    httpd_register_uri_handler(s_server, &wifi_get);
    httpd_register_uri_handler(s_server, &wifi);
    httpd_register_uri_handler(s_server, &wifi_scan);
    httpd_register_uri_handler(s_server, &sensors_get);
    httpd_register_uri_handler(s_server, &sensors_post);
    httpd_register_uri_handler(s_server, &ota_get);
    httpd_register_uri_handler(s_server, &ota_post);
    httpd_register_uri_handler(s_server, &logs_get);
    httpd_register_uri_handler(s_server, &logs_post);
}

static esp_err_t start_server(const char *index_page, bool provision_mode)
{
    if (s_server && (strcmp(s_index_page, index_page) != 0 || s_provision_mode != provision_mode)) {
        web_server_stop();
    }

    if (s_server) {
        return ESP_OK;
    }

    s_index_page = index_page;
    s_provision_mode = provision_mode;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 32;
    config.stack_size = 10240;
    /* OTA 固件上传可能较慢（尤其 SoftAP），拉长收发等待避免中途超时 */
    config.recv_wait_timeout = 120;
    config.send_wait_timeout = 120;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.lru_purge_enable = true;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    register_api_handlers();

    if (provision_mode) {
        err = web_server_register_static_routes();
        if (err != ESP_OK) {
            return err;
        }
        httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, captive_redirect_handler);
        ESP_LOGI(TAG, "provision server: http://192.168.4.1");
    } else {
        ESP_LOGI(TAG, "dashboard httpd ready (register MCP before static routes)");
    }

    return ESP_OK;
}

httpd_handle_t web_server_get_handle(void)
{
    return s_server;
}

esp_err_t web_server_register_static_routes(void)
{
    if (!s_server) {
        return ESP_ERR_INVALID_STATE;
    }

    httpd_uri_t static_files = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = static_file_handler,
    };
    esp_err_t err = httpd_register_uri_handler(s_server, &static_files);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "static route register failed: %s", esp_err_to_name(err));
        return err;
    }

    if (!s_provision_mode) {
        ESP_LOGI(TAG, "dashboard server started");
    }
    return ESP_OK;
}

esp_err_t web_server_start_provision(void)
{
    return start_server("index.html", true);
}

esp_err_t web_server_start_dashboard(void)
{
    return start_server("index.html", false);
}

void web_server_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
}
