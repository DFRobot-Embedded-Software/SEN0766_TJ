/**
 * @file mcp_provider_radar.c
 * @brief C4004 雷达 MCP Provider（配置/坐标工具 + 事件资源）
 */
#include "mcp_provider_radar.h"

#include "mcp_core.h"
#include "mcp_json_util.h"
#include "radar_area.h"
#include "radar_events.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_mcp_resource.h"
#include "esp_mcp_tool.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <math.h>

static const char *TAG = "mcp_radar";

#define MCP_AREA_SYNC_STACK 10240

static double round1(double v)
{
    return round(v * 10.0) / 10.0;
}

typedef struct {
    esp_mcp_resource_t *resource;
    char uri[48];
} mcp_dynamic_area_resource_t;

static mcp_dynamic_area_resource_t s_area_resources[AREA_TAG_MAX];
static uint8_t s_area_resource_count;
static QueueHandle_t s_area_sync_queue;
static TaskHandle_t s_area_sync_task;
static volatile bool s_runtime;

static area_config_t s_sync_cfg;
static char s_sync_title[AREA_TAG_NAME_LEN + 16];

static cJSON *area_config_to_json(const area_config_t *cfg)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }

    cJSON *tags = cJSON_AddArrayToObject(root, "tags");
    for (uint8_t i = 0; i < cfg->tag_count; i++) {
        const area_tag_t *tag = &cfg->tags[i];
        cJSON *item = cJSON_CreateObject();
        if (item == NULL) {
            continue;
        }
        cJSON_AddNumberToObject(item, "id", i);
        cJSON_AddNumberToObject(item, "strategy", tag->strategy);
        cJSON_AddStringToObject(item, "name", tag->name);
        cJSON *coords = cJSON_AddArrayToObject(item, "coords");
        for (int p = 0; p < 4; p++) {
            cJSON *pt = cJSON_CreateArray();
            if (pt == NULL) {
                continue;
            }
            cJSON_AddItemToArray(pt, cJSON_CreateNumber(tag->coords[p][0]));
            cJSON_AddItemToArray(pt, cJSON_CreateNumber(tag->coords[p][1]));
            cJSON_AddItemToArray(coords, pt);
        }
        cJSON_AddItemToArray(tags, item);
    }
    return root;
}

static esp_mcp_value_t get_config_callback(const esp_mcp_property_list_t *properties)
{
    (void)properties;
    area_config_t cfg = {0};
    if (area_config_get(&cfg) != ESP_OK) {
        return esp_mcp_value_create_bool(false);
    }
    return mcp_json_to_value(area_config_to_json(&cfg));
}

static esp_mcp_value_t fetch_radar_coord_callback(const esp_mcp_property_list_t *properties)
{
    (void)properties;
    float x = 0.0f, y = 0.0f;
    uint8_t target_count = 0;
    int8_t picked_index = -1;

    if (radar_area_fetch_coord(&x, &y, &target_count, &picked_index) != ESP_OK) {
        return esp_mcp_value_create_bool(false);
    }

    cJSON *json = cJSON_CreateObject();
    if (json == NULL) {
        return esp_mcp_value_create_bool(false);
    }
    cJSON_AddNumberToObject(json, "x_cm", round1(x));
    cJSON_AddNumberToObject(json, "y_cm", round1(y));
    cJSON_AddNumberToObject(json, "target_count", target_count);
    cJSON_AddNumberToObject(json, "picked_index", picked_index);
    return mcp_json_to_value(json);
}

static esp_err_t radar_events_feed_read_cb(const char *uri,
                                           char **out_mime_type,
                                           char **out_text,
                                           char **out_blob_base64,
                                           void *user_ctx)
{
    (void)uri;
    (void)user_ctx;
    (void)out_blob_base64;

    char *payload = NULL;
    if (radar_events_read_feed(&payload) != ESP_OK || payload == NULL) {
        return ESP_ERR_NO_MEM;
    }
    *out_mime_type = strdup("application/json");
    *out_text = payload;
    if (*out_mime_type == NULL) {
        free(payload);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t radar_area_events_read_cb(const char *uri,
                                           char **out_mime_type,
                                           char **out_text,
                                           char **out_blob_base64,
                                           void *user_ctx)
{
    (void)uri;
    (void)out_blob_base64;

    uint8_t area_id = (uint8_t)(uintptr_t)user_ctx;
    char *payload = NULL;
    esp_err_t err = radar_events_read_area(area_id, &payload);

    cJSON *json = NULL;
    if (err == ESP_ERR_NOT_FOUND) {
        json = cJSON_CreateObject();
        if (json != NULL) {
            cJSON_AddBoolToObject(json, "ready", false);
            cJSON_AddNumberToObject(json, "area_id", area_id);
        }
    } else if (err == ESP_OK && payload != NULL) {
        *out_text = payload;
        *out_mime_type = strdup("application/json");
        if (*out_mime_type == NULL) {
            free(payload);
            return ESP_ERR_NO_MEM;
        }
        return ESP_OK;
    }

    if (json == NULL) {
        free(payload);
        return err == ESP_OK ? ESP_ERR_NO_MEM : err;
    }

    payload = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (payload == NULL) {
        return ESP_ERR_NO_MEM;
    }
    *out_mime_type = strdup("application/json");
    *out_text = payload;
    if (*out_mime_type == NULL) {
        free(payload);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void clear_area_resources(void)
{
    esp_mcp_t *mcp = mcp_core_get_mcp();
    if (mcp == NULL) {
        s_area_resource_count = 0;
        return;
    }

    for (uint8_t i = 0; i < s_area_resource_count; i++) {
        if (s_area_resources[i].resource != NULL) {
            esp_mcp_remove_resource(mcp, s_area_resources[i].resource);
            esp_mcp_resource_destroy(s_area_resources[i].resource);
            s_area_resources[i].resource = NULL;
            s_area_resources[i].uri[0] = '\0';
        }
    }
    s_area_resource_count = 0;
    memset(s_area_resources, 0, sizeof(s_area_resources));
}

static void sync_area_resources(void)
{
    esp_mcp_t *mcp = mcp_core_get_mcp();
    if (mcp == NULL) {
        return;
    }

    clear_area_resources();

    if (area_config_get(&s_sync_cfg) != ESP_OK || s_sync_cfg.tag_count == 0) {
        ESP_LOGI(TAG, "area event resources cleared");
        return;
    }

    for (uint8_t i = 0; i < s_sync_cfg.tag_count; i++) {
        const area_tag_t *tag = &s_sync_cfg.tags[i];
        mcp_dynamic_area_resource_t *slot = &s_area_resources[i];

        snprintf(slot->uri, sizeof(slot->uri), MCP_RESOURCE_AREA_EVENTS_URI_FMT, i);
        snprintf(s_sync_title, sizeof(s_sync_title), "Events: %s", tag->name);

        slot->resource = esp_mcp_resource_create(
            slot->uri,
            s_sync_title,
            s_sync_title,
            "Radar events for this tag area. Subscribe; read after updated.",
            "application/json",
            radar_area_events_read_cb,
            (void *)(uintptr_t)i);
        if (slot->resource == NULL) {
            continue;
        }

        if (esp_mcp_add_resource(mcp, slot->resource) != ESP_OK) {
            esp_mcp_resource_destroy(slot->resource);
            slot->resource = NULL;
            slot->uri[0] = '\0';
            continue;
        }
    }

    s_area_resource_count = s_sync_cfg.tag_count;
    ESP_LOGI(TAG, "synced %u dynamic area event resources", s_area_resource_count);
}

static void run_area_sync(void)
{
    sync_area_resources();
    mcp_core_notify_resource(MCP_RESOURCE_EVENTS_URI);
}

static void area_sync_task(void *arg)
{
    (void)arg;
    while (true) {
        uint8_t trigger = 0;
        if (xQueueReceive(s_area_sync_queue, &trigger, pdMS_TO_TICKS(500)) != pdTRUE) {
            if (!s_runtime) {
                break;
            }
            continue;
        }
        while (xQueueReceive(s_area_sync_queue, &trigger, 0) == pdTRUE) {
        }
        if (!s_runtime) {
            break;
        }
        run_area_sync();
    }
    s_area_sync_task = NULL;
    vTaskDelete(NULL);
}

static void request_area_sync(void)
{
    if (s_area_sync_queue == NULL) {
        return;
    }
    uint8_t trigger = 1;
    (void)xQueueSend(s_area_sync_queue, &trigger, 0);
}

static void on_area_config_changed(void)
{
    radar_events_reset_state();
    radar_events_clear_log();
    request_area_sync();
}

static void on_radar_event(uint8_t area_id)
{
    if (area_id == RADAR_EVENT_AREA_ID_GLOBAL) {
        mcp_core_notify_resource(MCP_RESOURCE_EVENTS_URI);
        return;
    }
    if (area_id < s_area_resource_count && s_area_resources[area_id].resource != NULL) {
        mcp_core_notify_resource(s_area_resources[area_id].uri);
        return;
    }
    mcp_core_notify_resource(MCP_RESOURCE_EVENTS_URI);
}

static esp_err_t radar_register_tools(esp_mcp_t *mcp)
{
    esp_mcp_tool_t *tool = esp_mcp_tool_create(
        "sensors.get_config",
        "Read persisted C4004 area tags, strategies, and corner coordinates.",
        get_config_callback);
    ESP_RETURN_ON_FALSE(tool, ESP_ERR_NO_MEM, TAG, "create config tool failed");
    ESP_RETURN_ON_ERROR(esp_mcp_add_tool(mcp, tool), TAG, "add config tool failed");

    tool = esp_mcp_tool_create(
        "sensors.fetch_radar_coord",
        "Query current C4004 target position; when multiple targets exist, prefer motion.",
        fetch_radar_coord_callback);
    ESP_RETURN_ON_FALSE(tool, ESP_ERR_NO_MEM, TAG, "create coord tool failed");
    return esp_mcp_add_tool(mcp, tool);
}

static esp_err_t radar_register_resources(esp_mcp_t *mcp)
{
    esp_mcp_resource_t *resource = esp_mcp_resource_create(
        MCP_RESOURCE_EVENTS_URI,
        "radar-events",
        "Radar event stream",
        "All C4004 radar events with current area list. "
        "Also subscribe to sensor://area/{id}/events for a single area.",
        "application/json",
        radar_events_feed_read_cb,
        NULL);
    ESP_RETURN_ON_FALSE(resource, ESP_ERR_NO_MEM, TAG, "create events resource failed");
    return esp_mcp_add_resource(mcp, resource);
}

static esp_err_t radar_start_runtime(void)
{
    s_runtime = true;

    s_area_sync_queue = xQueueCreate(4, sizeof(uint8_t));
    if (s_area_sync_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(area_sync_task, "mcp_area_sync", MCP_AREA_SYNC_STACK, NULL, 3,
                    &s_area_sync_task) != pdPASS) {
        vQueueDelete(s_area_sync_queue);
        s_area_sync_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    area_config_set_change_callback(on_area_config_changed);
    request_area_sync();

    esp_err_t err = radar_events_start(on_radar_event);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "radar event watch failed: %s", esp_err_to_name(err));
    }
    return ESP_OK;
}

static void radar_stop_runtime(void)
{
    s_runtime = false;
    area_config_set_change_callback(NULL);

    if (s_area_sync_queue != NULL) {
        uint8_t trigger = 1;
        (void)xQueueSend(s_area_sync_queue, &trigger, 0);
    }
    while (s_area_sync_task != NULL) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (s_area_sync_queue != NULL) {
        vQueueDelete(s_area_sync_queue);
        s_area_sync_queue = NULL;
    }

    radar_events_stop();
    clear_area_resources();
}

const mcp_sensor_provider_t mcp_provider_radar = {
    .name = "radar",
    .register_tools = radar_register_tools,
    .register_resources = radar_register_resources,
    .start_runtime = radar_start_runtime,
    .stop_runtime = radar_stop_runtime,
};
