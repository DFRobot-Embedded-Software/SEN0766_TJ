#include "sensor_data.h"
#include "stcc4_sensor.h"
#include "c4004_sensor.h"

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "sensor_data";

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
    if (s_c4004_started || !s_area_config.enabled) {
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

esp_err_t area_coords_to_rect_for_radar(const area_tag_t *tag, area_rect_t *out)
{
    /* 边缘门与统计/靠近远离相同，使用用户标定的完整矩形（官方 readTagEvent 示例亦如此） */
    return area_coords_to_rect(tag, out);
}

esp_err_t sensor_data_fetch_radar_coord(float *x, float *y, uint8_t *target_count, int8_t *picked_index)
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

    cJSON_AddBoolToObject(root, "enabled", s_area_config.enabled);
    cJSON *tags = cJSON_AddArrayToObject(root, "tags");
    for (uint8_t i = 0; i < s_area_config.tag_count; i++) {
        const area_tag_t *tag = &s_area_config.tags[i];
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "strategy", tag->strategy);
        cJSON_AddStringToObject(item, "name", tag->name);
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

    const cJSON *enabled = cJSON_GetObjectItem(root, "enabled");
    if (cJSON_IsBool(enabled)) {
        s_area_config.enabled = cJSON_IsTrue(enabled);
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
            tag->strategy = (uint8_t)strategy->valueint;
            strncpy(tag->name, name->valuestring, sizeof(tag->name) - 1);
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

void sensor_data_init(void)
{
    area_config_load();

    esp_err_t err = stcc4_sensor_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "STCC4 init failed: %s", esp_err_to_name(err));
    }

    if (s_area_config.enabled) {
        if (xTaskCreate(c4004_boot_task, "c4004_boot", C4004_BOOT_TASK_STACK, NULL, 3, NULL) != pdPASS) {
            ESP_LOGW(TAG, "C4004 boot task create failed");
        }
    }
}

esp_err_t sensor_data_get_snapshot(sensor_snapshot_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));

    if (stcc4_sensor_get(&out->temperature, &out->humidity, &out->co2) == ESP_OK) {
        out->env_ready = true;
    }

    if (!s_area_config.enabled || s_area_config.tag_count == 0) {
        return ESP_OK;
    }

    out->area_count = s_area_config.tag_count;

    if (c4004_sensor_is_ready()) {
        (void)c4004_sensor_refresh_targets();
    }

    for (uint8_t i = 0; i < s_area_config.tag_count; i++) {
        const area_tag_t *tag = &s_area_config.tags[i];
        area_status_t *st = &out->areas[i];

        strncpy(st->name, tag->name, AREA_TAG_NAME_LEN - 1);
        st->strategy = tag->strategy;

        if (c4004_sensor_get_tag_status(i, st) != ESP_OK) {
            st->strategy = tag->strategy;
        }

        if (tag->strategy == 1 && c4004_sensor_is_ready()) {
            area_rect_t rect = {0};
            if (area_coords_to_rect_for_radar(tag, &rect) == ESP_OK) {
                (void)c4004_sensor_refine_people_count(i, &rect, st);
            }
        }
    }
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

esp_err_t area_config_set_enabled(bool enabled)
{
    s_area_config.enabled = enabled;
    esp_err_t err = area_config_save();
    if (err == ESP_OK && enabled) {
        c4004_sync_if_ready(0);
    }
    if (err == ESP_OK) {
        notify_area_config_changed();
    }
    return err;
}

esp_err_t area_config_add_tag(const area_tag_t *tag)
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
    area_coords_to_rect_for_radar(tag, &rect);
    ESP_LOGI(TAG, "add tag \"%s\" strategy=%u rect %ux%u cm at (%d,%d)",
             tag->name, tag->strategy,
             (unsigned)rect.width, (unsigned)rect.height,
             (int)rect.center_x, (int)rect.center_y);

    uint8_t prev_count = s_area_config.tag_count;
    s_area_config.tags[s_area_config.tag_count] = *tag;
    s_area_config.tag_count++;
    esp_err_t err = area_config_save();
    if (err == ESP_OK) {
        c4004_sync_if_ready(prev_count);
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
