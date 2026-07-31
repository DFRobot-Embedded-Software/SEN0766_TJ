#include "radar_area.h"
#include "c4004_sensor.h"

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "radar_area";

#define NVS_NAMESPACE "area_cfg"
#define NVS_KEY_JSON  "json_v2"

static area_config_t s_area_config;
static bool s_c4004_started;
static area_config_changed_cb_t s_area_config_changed_cb;

static void notify_area_config_changed(void)
{
    if (s_area_config_changed_cb != NULL) {
        s_area_config_changed_cb();
    }
}

#define C4004_BOOT_TASK_STACK 6144

static void c4004_ensure_started(void)
{
    if (s_c4004_started) {
        return;
    }
    if (c4004_sensor_start() == ESP_OK) {
        s_c4004_started = true;
    }
}

static void c4004_sync_if_ready(uint8_t prev_count)
{
    c4004_ensure_started();
    if (c4004_sensor_is_ready()) {
        esp_err_t err = c4004_sensor_sync_tags(&s_area_config, prev_count);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "C4004 sync failed: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGW(TAG, "C4004 not ready, skip tag sync");
    }
}

static void c4004_boot_task(void *arg)
{
    (void)arg;
    /* 启动雷达；有标签则同步，无标签只清模块侧残留 */
    c4004_sync_if_ready(0);
    vTaskDelete(NULL);
}

bool area_coords_valid(const area_tag_t *tag)
{
    if (tag == NULL) {
        return false;
    }

    bool has_point = false;
    for (int i = 0; i < 4; i++) {
        if (tag->coords[i][0] != 0.0f || tag->coords[i][1] != 0.0f) {
            has_point = true;
            break;
        }
    }
    if (!has_point) {
        return false;
    }

    area_rect_t rect = {0};
    return area_coords_to_rect(tag, &rect) == ESP_OK;
}

esp_err_t area_coords_to_rect(const area_tag_t *tag, area_rect_t *out)
{
    if (tag == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    float min_x = tag->coords[0][0];
    float max_x = tag->coords[0][0];
    float min_y = tag->coords[0][1];
    float max_y = tag->coords[0][1];

    for (int i = 1; i < 4; i++) {
        if (tag->coords[i][0] < min_x) {
            min_x = tag->coords[i][0];
        }
        if (tag->coords[i][0] > max_x) {
            max_x = tag->coords[i][0];
        }
        if (tag->coords[i][1] < min_y) {
            min_y = tag->coords[i][1];
        }
        if (tag->coords[i][1] > max_y) {
            max_y = tag->coords[i][1];
        }
    }

    float width_f = max_x - min_x;
    float height_f = max_y - min_y;
    if (width_f < AREA_RECT_MIN_SIZE_CM || height_f < AREA_RECT_MIN_SIZE_CM) {
        ESP_LOGW(TAG,
                 "rect too small: bounds (%.0f,%.0f)-(%.0f,%.0f) size %.0fx%.0f cm (min %d)",
                 min_x, min_y, max_x, max_y, width_f, height_f, AREA_RECT_MIN_SIZE_CM);
        return ESP_ERR_INVALID_SIZE;
    }

    out->min_x = min_x;
    out->min_y = min_y;
    out->max_x = max_x;
    out->max_y = max_y;
    out->center_x = (int16_t)((min_x + max_x) / 2.0f);
    out->center_y = (int16_t)((min_y + max_y) / 2.0f);
    out->width = (uint16_t)width_f;
    out->height = (uint16_t)height_f;

    ESP_LOGI(TAG,
             "coords->rect: P1(%.0f,%.0f) P2(%.0f,%.0f) P3(%.0f,%.0f) P4(%.0f,%.0f)"
             " => center(%d,%d) size %ux%u cm",
             tag->coords[0][0], tag->coords[0][1],
             tag->coords[1][0], tag->coords[1][1],
             tag->coords[2][0], tag->coords[2][1],
             tag->coords[3][0], tag->coords[3][1],
             (int)out->center_x, (int)out->center_y,
             (unsigned)out->width, (unsigned)out->height);
    return ESP_OK;
}

bool area_point_in_noise_zone(float x_cm, float y_cm)
{
    for (uint8_t i = 0; i < s_area_config.tag_count; i++) {
        const area_tag_t *tag = &s_area_config.tags[i];
        if (tag->strategy != 4) {
            continue;
        }
        float min_x = tag->coords[0][0];
        float max_x = tag->coords[0][0];
        float min_y = tag->coords[0][1];
        float max_y = tag->coords[0][1];
        for (int p = 1; p < 4; p++) {
            if (tag->coords[p][0] < min_x) {
                min_x = tag->coords[p][0];
            }
            if (tag->coords[p][0] > max_x) {
                max_x = tag->coords[p][0];
            }
            if (tag->coords[p][1] < min_y) {
                min_y = tag->coords[p][1];
            }
            if (tag->coords[p][1] > max_y) {
                max_y = tag->coords[p][1];
            }
        }
        if ((max_x - min_x) < AREA_RECT_MIN_SIZE_CM ||
            (max_y - min_y) < AREA_RECT_MIN_SIZE_CM) {
            continue;
        }
        if (x_cm >= min_x && x_cm <= max_x && y_cm >= min_y && y_cm <= max_y) {
            return true;
        }
    }
    return false;
}

bool area_point_in_detection_range(float x_cm, float y_cm)
{
    if (!s_area_config.has_detection_range) {
        return true;
    }

    float min_x = s_area_config.detection_range[0][0];
    float max_x = s_area_config.detection_range[0][0];
    float min_y = s_area_config.detection_range[0][1];
    float max_y = s_area_config.detection_range[0][1];
    for (int p = 1; p < 4; p++) {
        if (s_area_config.detection_range[p][0] < min_x) {
            min_x = s_area_config.detection_range[p][0];
        }
        if (s_area_config.detection_range[p][0] > max_x) {
            max_x = s_area_config.detection_range[p][0];
        }
        if (s_area_config.detection_range[p][1] < min_y) {
            min_y = s_area_config.detection_range[p][1];
        }
        if (s_area_config.detection_range[p][1] > max_y) {
            max_y = s_area_config.detection_range[p][1];
        }
    }
    if ((max_x - min_x) < AREA_RECT_MIN_SIZE_CM ||
        (max_y - min_y) < AREA_RECT_MIN_SIZE_CM) {
        return true;
    }
    /* 边缘留 15cm，避免轨迹抖动被判在范围外 */
    const float margin = 15.0f;
    return x_cm >= (min_x - margin) && x_cm <= (max_x + margin) &&
           y_cm >= (min_y - margin) && y_cm <= (max_y + margin);
}

esp_err_t radar_area_fetch_coord(float *x, float *y, uint8_t *target_count, int8_t *picked_index)
{
    if (x == NULL || y == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    c4004_ensure_started();
    if (!c4004_sensor_is_ready()) {
        ESP_LOGW(TAG, "fetch coord: C4004 not ready");
        return ESP_ERR_INVALID_STATE;
    }

    return c4004_sensor_query_target(x, y, target_count, picked_index);
}

static esp_err_t area_config_save(void)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddBoolToObject(root, "has_detection_range", s_area_config.has_detection_range);
    if (s_area_config.has_detection_range) {
        cJSON *range = cJSON_AddArrayToObject(root, "detection_range");
        for (int p = 0; p < 4; p++) {
            cJSON *pt = cJSON_CreateArray();
            cJSON_AddItemToArray(pt, cJSON_CreateNumber(s_area_config.detection_range[p][0]));
            cJSON_AddItemToArray(pt, cJSON_CreateNumber(s_area_config.detection_range[p][1]));
            cJSON_AddItemToArray(range, pt);
        }
    }
    cJSON *tags = cJSON_AddArrayToObject(root, "tags");
    for (uint8_t i = 0; i < s_area_config.tag_count; i++) {
        const area_tag_t *tag = &s_area_config.tags[i];
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "strategy", tag->strategy);
        cJSON_AddStringToObject(item, "name", tag->name);
        if (tag->setting[0] != '\0') {
            cJSON_AddStringToObject(item, "setting", tag->setting);
        }
        cJSON *coords = cJSON_AddArrayToObject(item, "coords");
        for (int p = 0; p < 4; p++) {
            cJSON *pt = cJSON_CreateArray();
            cJSON_AddItemToArray(pt, cJSON_CreateNumber(tag->coords[p][0]));
            cJSON_AddItemToArray(pt, cJSON_CreateNumber(tag->coords[p][1]));
            cJSON_AddItemToArray(coords, pt);
        }
        cJSON_AddItemToArray(tags, item);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, NVS_KEY_JSON, json);
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    free(json);
    return err;
}

static bool parse_coord_point(const cJSON *pt, float *x, float *y)
{
    if (!cJSON_IsArray(pt) || cJSON_GetArraySize(pt) < 2) {
        return false;
    }
    const cJSON *jx = cJSON_GetArrayItem(pt, 0);
    const cJSON *jy = cJSON_GetArrayItem(pt, 1);
    if (!cJSON_IsNumber(jx) || !cJSON_IsNumber(jy)) {
        return false;
    }
    *x = (float)jx->valuedouble;
    *y = (float)jy->valuedouble;
    return true;
}

static esp_err_t area_config_load(void)
{
    memset(&s_area_config, 0, sizeof(s_area_config));

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t len = 0;
    err = nvs_get_str(handle, NVS_KEY_JSON, NULL, &len);
    if (err != ESP_OK || len == 0) {
        nvs_close(handle);
        return err;
    }

    char *buf = malloc(len);
    if (buf == NULL) {
        nvs_close(handle);
        return ESP_ERR_NO_MEM;
    }

    err = nvs_get_str(handle, NVS_KEY_JSON, buf, &len);
    nvs_close(handle);
    if (err != ESP_OK) {
        free(buf);
        return err;
    }

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (root == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const cJSON *has_range = cJSON_GetObjectItem(root, "has_detection_range");
    const cJSON *range = cJSON_GetObjectItem(root, "detection_range");
    if (cJSON_IsTrue(has_range) && cJSON_IsArray(range) && cJSON_GetArraySize(range) >= 4) {
        bool ok = true;
        for (int p = 0; p < 4; p++) {
            if (!parse_coord_point(cJSON_GetArrayItem(range, p),
                                   &s_area_config.detection_range[p][0],
                                   &s_area_config.detection_range[p][1])) {
                ok = false;
                break;
            }
        }
        s_area_config.has_detection_range = ok;
    }

    const cJSON *tags = cJSON_GetObjectItem(root, "tags");
    if (cJSON_IsArray(tags)) {
        int count = cJSON_GetArraySize(tags);
        if (count > AREA_TAG_MAX) {
            count = AREA_TAG_MAX;
        }
        for (int i = 0; i < count; i++) {
            const cJSON *item = cJSON_GetArrayItem(tags, i);
            const cJSON *strategy = cJSON_GetObjectItem(item, "strategy");
            const cJSON *name = cJSON_GetObjectItem(item, "name");
            const cJSON *coords = cJSON_GetObjectItem(item, "coords");
            if (!cJSON_IsNumber(strategy) || !cJSON_IsString(name) || !cJSON_IsArray(coords)) {
                continue;
            }
            area_tag_t *tag = &s_area_config.tags[s_area_config.tag_count];
            memset(tag, 0, sizeof(*tag));
            tag->strategy = (uint8_t)strategy->valueint;
            strncpy(tag->name, name->valuestring, sizeof(tag->name) - 1);
            const cJSON *setting = cJSON_GetObjectItem(item, "setting");
            if (cJSON_IsString(setting) && setting->valuestring != NULL) {
                strncpy(tag->setting, setting->valuestring, sizeof(tag->setting) - 1);
            }
            for (int p = 0; p < 4 && p < cJSON_GetArraySize(coords); p++) {
                parse_coord_point(cJSON_GetArrayItem(coords, p),
                                  &tag->coords[p][0], &tag->coords[p][1]);
            }
            s_area_config.tag_count++;
        }
    }

    cJSON_Delete(root);
    return ESP_OK;
}

void radar_area_init(void)
{
    area_config_load();
    ESP_LOGI(TAG, "area cfg: tags=%u has_range=%d",
             (unsigned)s_area_config.tag_count, (int)s_area_config.has_detection_range);

    /* 始终拉起 C4004（坐标/概览不依赖区域开关）；标签有则同步 */
    if (xTaskCreate(c4004_boot_task, "c4004_boot", C4004_BOOT_TASK_STACK, NULL, 3, NULL) != pdPASS) {
        ESP_LOGW(TAG, "C4004 boot task create failed");
    }
}

esp_err_t radar_area_get_status(area_status_t areas[AREA_TAG_MAX], uint8_t *out_count)
{
    if (areas == NULL || out_count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_count = 0;
    if (s_area_config.tag_count == 0) {
        return ESP_OK;
    }

    if (c4004_sensor_is_ready()) {
        (void)c4004_sensor_refresh_targets();
    }

    for (uint8_t i = 0; i < s_area_config.tag_count; i++) {
        const area_tag_t *tag = &s_area_config.tags[i];
        area_status_t *st = &areas[i];

        memset(st, 0, sizeof(*st));
        strncpy(st->name, tag->name, AREA_TAG_NAME_LEN - 1);
        st->strategy = tag->strategy;

        if (c4004_sensor_get_tag_status(i, st) != ESP_OK) {
            st->strategy = tag->strategy;
        }
        /* 配置侧策略为准；干扰源/空标签强制不计入人数 */
        st->strategy = tag->strategy;
        if (tag->strategy == 4 || tag->strategy == 5) {
            st->active = 0;
            st->static_count = 0;
            st->enter = false;
            st->exit = false;
            st->approaching = false;
            st->leaving = false;
        }

        if (tag->strategy == 1 && c4004_sensor_is_ready()) {
            area_rect_t rect = {0};
            if (area_coords_to_rect(tag, &rect) == ESP_OK) {
                (void)c4004_sensor_refine_people_count(i, &rect, st);
            }
        }
    }

    *out_count = s_area_config.tag_count;
    return ESP_OK;
}

esp_err_t area_config_get(area_config_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = s_area_config;
    return ESP_OK;
}

esp_err_t area_config_add_tag(const area_tag_t *tag, uint8_t *out_id)
{
    if (tag == NULL || tag->name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (!area_coords_valid(tag)) {
        ESP_LOGW(TAG, "add tag rejected: invalid coords for \"%s\"", tag->name);
        return ESP_ERR_INVALID_ARG;
    }
    if (s_area_config.tag_count >= AREA_TAG_MAX) {
        return ESP_ERR_NO_MEM;
    }

    area_rect_t rect = {0};
    area_coords_to_rect(tag, &rect);
    ESP_LOGI(TAG, "add tag \"%s\" strategy=%u rect %ux%u cm at (%d,%d)",
             tag->name, tag->strategy,
             (unsigned)rect.width, (unsigned)rect.height,
             (int)rect.center_x, (int)rect.center_y);

    uint8_t prev_count = s_area_config.tag_count;
    s_area_config.tags[s_area_config.tag_count] = *tag;
    if (out_id != NULL) {
        *out_id = s_area_config.tag_count;
    }
    s_area_config.tag_count++;
    esp_err_t err = area_config_save();
    if (err == ESP_OK) {
        c4004_sync_if_ready(prev_count);
        notify_area_config_changed();
    }
    return err;
}

esp_err_t area_config_update_tag(uint8_t index, const area_tag_t *tag)
{
    if (tag == NULL || tag->name[0] == '\0' || index >= s_area_config.tag_count) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!area_coords_valid(tag)) {
        return ESP_ERR_INVALID_ARG;
    }

    s_area_config.tags[index] = *tag;
    esp_err_t err = area_config_save();
    if (err == ESP_OK) {
        c4004_sync_if_ready(s_area_config.tag_count);
        notify_area_config_changed();
    }
    return err;
}

esp_err_t area_config_set_detection_range(const float coords[4][2])
{
    if (coords == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    area_tag_t probe = {0};
    memcpy(probe.coords, coords, sizeof(probe.coords));
    if (!area_coords_valid(&probe)) {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(s_area_config.detection_range, coords, sizeof(s_area_config.detection_range));
    s_area_config.has_detection_range = true;

    esp_err_t err = area_config_save();
    if (err == ESP_OK) {
        c4004_sync_if_ready(s_area_config.tag_count);
        notify_area_config_changed();
    }
    return err;
}

esp_err_t area_config_remove_tag(uint8_t index)
{
    if (index >= s_area_config.tag_count) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "remove tag[%u] \"%s\", syncing C4004", index, s_area_config.tags[index].name);

    uint8_t prev_count = s_area_config.tag_count;
    for (uint8_t i = index; i + 1 < s_area_config.tag_count; i++) {
        s_area_config.tags[i] = s_area_config.tags[i + 1];
    }
    s_area_config.tag_count--;
    esp_err_t err = area_config_save();
    if (err == ESP_OK) {
        c4004_sync_if_ready(prev_count);
        notify_area_config_changed();
    }
    return err;
}

void area_config_set_change_callback(area_config_changed_cb_t cb)
{
    s_area_config_changed_cb = cb;
}
