/**
 * @file mcp_provider_env.c
 * @brief 环境传感 MCP Provider（snapshot + sensor://temperature）
 */
#include "mcp_provider_env.h"

#include "board_snapshot.h"
#include "env_sensors.h"
#include "env_sound.h"
#include "mcp_core.h"
#include "mcp_json_util.h"
#include "sensor_settings.h"

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
#define MCP_TEMP_WATCH_STACK        8192

static TaskHandle_t s_temp_watch_task;
static volatile bool s_runtime;

static double round1(double v)
{
    return round(v * 10.0) / 10.0;
}

static cJSON *snapshot_to_json(const sensor_snapshot_t *snapshot)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }

    cJSON_AddBoolToObject(root, "env_ready", snapshot->env_ready);
    if (snapshot->env_ready) {
        cJSON_AddNumberToObject(root, "temperature_c", round1(snapshot->temperature));
        cJSON_AddNumberToObject(root, "humidity_pct", round1(snapshot->humidity));
        cJSON_AddNumberToObject(root, "co2_ppm", snapshot->co2);
    } else {
        cJSON_AddNullToObject(root, "temperature_c");
        cJSON_AddNullToObject(root, "humidity_pct");
        cJSON_AddNullToObject(root, "co2_ppm");
    }

    cJSON_AddBoolToObject(root, "lux_ready", snapshot->lux_ready);
    if (snapshot->lux_ready) {
        cJSON_AddNumberToObject(root, "lux", (int)lround(snapshot->lux));
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
        case 4:
            cJSON_AddStringToObject(area, "role", "noise");
            break;
        case 5:
            cJSON_AddStringToObject(area, "role", "none");
            break;
        default:
            cJSON_AddNumberToObject(area, "active", st->active);
            cJSON_AddNumberToObject(area, "static", st->static_count);
            break;
        }
        cJSON_AddItemToArray(areas, area);
    }

#if CONFIG_ENV_SOUND_ENABLE
    float sound_db = 0.0f;
    if (sensor_settings_sound_enabled() && env_sound_get_db(&sound_db)) {
        sound_db += sensor_settings_sound_offset_db();
        if (sound_db < 0.0f) {
            sound_db = 0.0f;
        } else if (sound_db > 140.0f) {
            sound_db = 140.0f;
        }
        cJSON_AddBoolToObject(root, "sound_ready", true);
        cJSON_AddNumberToObject(root, "sound_db", (int)lround(sound_db));
        cJSON_AddNumberToObject(root, "sound_warning_db",
                                (double)sensor_settings_sound_warning_db());
        cJSON_AddBoolToObject(root, "sound_alarm",
                              sound_db >= sensor_settings_sound_warning_db());
    } else {
        cJSON_AddBoolToObject(root, "sound_ready", false);
        cJSON_AddNullToObject(root, "sound_db");
        cJSON_AddNumberToObject(root, "sound_warning_db",
                                (double)sensor_settings_sound_warning_db());
        cJSON_AddBoolToObject(root, "sound_alarm", false);
    }
#else
    cJSON_AddBoolToObject(root, "sound_ready", false);
    cJSON_AddNullToObject(root, "sound_db");
    cJSON_AddNumberToObject(root, "sound_warning_db",
                            (double)sensor_settings_sound_warning_db());
    cJSON_AddBoolToObject(root, "sound_alarm", false);
#endif

    /* 附带异常判定阈值，避免再调一次 get_thresholds */
    cJSON *thresholds = sensor_settings_to_json();
    if (thresholds != NULL) {
        cJSON_AddItemToObject(root, "thresholds", thresholds);
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

static esp_mcp_value_t get_thresholds_callback(const esp_mcp_property_list_t *properties)
{
    (void)properties;
    cJSON *json = sensor_settings_to_json();
    if (json == NULL) {
        return esp_mcp_value_create_bool(false);
    }
    return mcp_json_to_value(json);
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
        cJSON_AddNumberToObject(json, "temperature_c", round1(snap.temperature));
        cJSON_AddNumberToObject(json, "humidity_pct", round1(snap.humidity));
        cJSON_AddNumberToObject(json, "co2_ppm", snap.co2);
    }
    cJSON_AddBoolToObject(json, "lux_ready", snap.lux_ready);
    if (snap.lux_ready) {
        cJSON_AddNumberToObject(json, "lux", (int)lround(snap.lux));
    }
#if CONFIG_ENV_SOUND_ENABLE
    float sound_db = 0.0f;
    if (sensor_settings_sound_enabled() && env_sound_get_db(&sound_db)) {
        sound_db += sensor_settings_sound_offset_db();
        cJSON_AddNumberToObject(json, "sound_db", (int)lround(sound_db));
    }
#endif
    cJSON_AddNumberToObject(json, "sound_warning_db",
                            (double)sensor_settings_sound_warning_db());
    cJSON *thresholds = sensor_settings_to_json();
    if (thresholds != NULL) {
        cJSON_AddItemToObject(json, "thresholds", thresholds);
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
        env_snapshot_t snap = {0};
        if (env_sensors_get(&snap) == ESP_OK && snap.env_ready) {
            bool changed = isnan(last_temperature) ||
                           fabsf(snap.temperature - last_temperature) >= MCP_TEMP_CHANGE_THRESHOLD_C;
            if (changed) {
                last_temperature = snap.temperature;
                mcp_core_notify_resource(MCP_RESOURCE_TEMPERATURE_URI);
                ESP_LOGI(TAG, "Temperature update pushed: %.2f C", (double)snap.temperature);
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
        "Read live environment: temp/humidity/CO2/lux/sound_db, area status, "
        "and thresholds (incl. sound.warning_db). "
        "Use this for questions about current values or alert thresholds.",
        get_snapshot_callback);
    ESP_RETURN_ON_FALSE(tool, ESP_ERR_NO_MEM, TAG, "create snapshot tool failed");
    ESP_RETURN_ON_ERROR(esp_mcp_add_tool(mcp, tool), TAG, "add snapshot tool failed");

    tool = esp_mcp_tool_create(
        "sensors.get_thresholds",
        "Read environment anomaly thresholds only: "
        "temperature_humidity.temp_high/humidity_high/humidity_low, "
        "co2.warning_ppm, sound.warning_db (dB), light.dark_lux/bright_lux. "
        "Same fields as Web sensor settings page.",
        get_thresholds_callback);
    ESP_RETURN_ON_FALSE(tool, ESP_ERR_NO_MEM, TAG, "create thresholds tool failed");
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
    if (xTaskCreate(temperature_watch_task, "mcp_temp_watch", MCP_TEMP_WATCH_STACK, NULL, 4,
                    &s_temp_watch_task) != pdPASS) {
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
