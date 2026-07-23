/**
 * @file mcp_provider_env.c
 * @brief 环境传感 MCP Provider（snapshot + sensor://temperature）
 */
#include "mcp_provider_env.h"

#include "board_snapshot.h"
#include "mcp_core.h"
#include "mcp_json_util.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_mcp_resource.h"
#include "esp_mcp_tool.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "mcp_env";

#define MCP_TEMP_CHANGE_THRESHOLD_C 0.1f

static TaskHandle_t s_temp_watch_task;
static volatile bool s_runtime;

static cJSON *snapshot_to_json(const sensor_snapshot_t *snapshot)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }

    cJSON_AddBoolToObject(root, "env_ready", snapshot->env_ready);
    if (snapshot->env_ready) {
        cJSON_AddNumberToObject(root, "temperature_c", snapshot->temperature);
        cJSON_AddNumberToObject(root, "humidity_pct", snapshot->humidity);
        cJSON_AddNumberToObject(root, "co2_ppm", snapshot->co2);
    } else {
        cJSON_AddNullToObject(root, "temperature_c");
        cJSON_AddNullToObject(root, "humidity_pct");
        cJSON_AddNullToObject(root, "co2_ppm");
    }

    cJSON_AddBoolToObject(root, "lux_ready", snapshot->lux_ready);
    if (snapshot->lux_ready) {
        cJSON_AddNumberToObject(root, "lux", snapshot->lux);
    } else {
        cJSON_AddNullToObject(root, "lux");
    }

    cJSON *areas = cJSON_AddArrayToObject(root, "areas");
    for (uint8_t i = 0; i < snapshot->area_count; i++) {
        const area_status_t *st = &snapshot->areas[i];
        cJSON *area = cJSON_CreateObject();
        if (area == NULL) {
            continue;
        }
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
        default:
            cJSON_AddNumberToObject(area, "active", st->active);
            cJSON_AddNumberToObject(area, "static", st->static_count);
            break;
        }
        cJSON_AddItemToArray(areas, area);
    }

    return root;
}

static esp_mcp_value_t get_snapshot_callback(const esp_mcp_property_list_t *properties)
{
    (void)properties;
    sensor_snapshot_t snapshot = {0};
    if (board_snapshot_get(&snapshot) != ESP_OK) {
        return esp_mcp_value_create_bool(false);
    }
    return mcp_json_to_value(snapshot_to_json(&snapshot));
}

static esp_err_t temperature_resource_read_cb(const char *uri,
                                              char **out_mime_type,
                                              char **out_text,
                                              char **out_blob_base64,
                                              void *user_ctx)
{
    (void)uri;
    (void)user_ctx;
    (void)out_blob_base64;

    sensor_snapshot_t snap = {0};
    (void)board_snapshot_get(&snap);

    cJSON *json = cJSON_CreateObject();
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddBoolToObject(json, "ready", snap.env_ready);
    if (snap.env_ready) {
        cJSON_AddNumberToObject(json, "temperature_c", snap.temperature);
        cJSON_AddNumberToObject(json, "humidity_pct", snap.humidity);
        cJSON_AddNumberToObject(json, "co2_ppm", snap.co2);
    }
    cJSON_AddBoolToObject(json, "lux_ready", snap.lux_ready);
    if (snap.lux_ready) {
        cJSON_AddNumberToObject(json, "lux", snap.lux);
    }

    char *payload = cJSON_PrintUnformatted(json);
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

static void temperature_watch_task(void *arg)
{
    (void)arg;
    float last_temperature = NAN;
    const TickType_t interval = pdMS_TO_TICKS(CONFIG_STCC4_POLL_INTERVAL_MS);

    while (s_runtime) {
        sensor_snapshot_t snap = {0};
        if (board_snapshot_get(&snap) == ESP_OK && snap.env_ready) {
            bool changed = isnan(last_temperature) ||
                           fabsf(snap.temperature - last_temperature) >= MCP_TEMP_CHANGE_THRESHOLD_C;
            if (changed) {
                last_temperature = snap.temperature;
                mcp_core_notify_resource(MCP_RESOURCE_TEMPERATURE_URI);
                ESP_LOGI(TAG, "Temperature update pushed: %.2f C", snap.temperature);
            }
        }
        vTaskDelay(interval);
    }

    s_temp_watch_task = NULL;
    vTaskDelete(NULL);
}

static esp_err_t env_register_tools(esp_mcp_t *mcp)
{
    esp_mcp_tool_t *tool = esp_mcp_tool_create(
        "sensors.get_snapshot",
        "Read environment (temp/humidity/CO2/lux) + area status. "
        "Accelerometer is read separately (not part of MCP reporting).",
        get_snapshot_callback);
    ESP_RETURN_ON_FALSE(tool, ESP_ERR_NO_MEM, TAG, "create snapshot tool failed");
    return esp_mcp_add_tool(mcp, tool);
}

static esp_err_t env_register_resources(esp_mcp_t *mcp)
{
    esp_mcp_resource_t *resource = esp_mcp_resource_create(
        MCP_RESOURCE_TEMPERATURE_URI,
        "temperature",
        "Temperature sensor",
        "Live environment readings (temp/humidity/CO2/lux). "
        "Subscribe for notifications/resources/updated on temperature change.",
        "application/json",
        temperature_resource_read_cb,
        NULL);
    ESP_RETURN_ON_FALSE(resource, ESP_ERR_NO_MEM, TAG, "create temperature resource failed");
    return esp_mcp_add_resource(mcp, resource);
}

static esp_err_t env_start_runtime(void)
{
    s_runtime = true;
    if (xTaskCreate(temperature_watch_task, "mcp_temp_watch", 3072, NULL, 4, &s_temp_watch_task) !=
        pdPASS) {
        s_temp_watch_task = NULL;
        ESP_LOGW(TAG, "temperature watch task failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void env_stop_runtime(void)
{
    s_runtime = false;
    while (s_temp_watch_task != NULL) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

const mcp_sensor_provider_t mcp_provider_env = {
    .name = "env",
    .register_tools = env_register_tools,
    .register_resources = env_register_resources,
    .start_runtime = env_start_runtime,
    .stop_runtime = env_stop_runtime,
};
