#include "sensor_settings.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "esp_log.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "sensor_settings";

#define NVS_NAMESPACE "sensor_cfg"
#define NVS_KEY_JSON  "json_v1"

static cJSON *s_sensors;

static cJSON *make_defaults(void)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }

    cJSON *mm = cJSON_AddObjectToObject(root, "mmwave");
    cJSON_AddBoolToObject(mm, "enabled", true);
    cJSON_AddNumberToObject(mm, "distance_m", 8);
    cJSON_AddStringToObject(mm, "sensitivity", "medium");
    cJSON_AddNumberToObject(mm, "static_seconds", 30);
    cJSON_AddNumberToObject(mm, "install_height_m", 1.8);
    cJSON_AddBoolToObject(mm, "multi_target", true);
    cJSON_AddBoolToObject(mm, "region_filter", true);

    cJSON *th = cJSON_AddObjectToObject(root, "temperature_humidity");
    cJSON_AddBoolToObject(th, "enabled", true);
    cJSON_AddNumberToObject(th, "period_s", 5);
    cJSON_AddNumberToObject(th, "temp_high", 30);
    cJSON_AddNumberToObject(th, "humidity_high", 80);
    cJSON_AddNumberToObject(th, "humidity_low", 20);
    cJSON_AddNumberToObject(th, "temp_offset", 0);
    cJSON_AddNumberToObject(th, "humidity_offset", 0);
    cJSON_AddBoolToObject(th, "alarm", true);

    cJSON *co2 = cJSON_AddObjectToObject(root, "co2");
    cJSON_AddBoolToObject(co2, "enabled", true);
    cJSON_AddNumberToObject(co2, "period_s", 5);
    cJSON_AddNumberToObject(co2, "warning_ppm", 1000);
    cJSON_AddNumberToObject(co2, "critical_ppm", 1500);
    cJSON_AddNumberToObject(co2, "offset_ppm", 0);
    cJSON_AddBoolToObject(co2, "alarm", true);

    cJSON *sound = cJSON_AddObjectToObject(root, "sound");
    cJSON_AddBoolToObject(sound, "enabled", true);
    cJSON_AddNumberToObject(sound, "period_s", 5);
    cJSON_AddNumberToObject(sound, "warning_db", 60);
    cJSON_AddNumberToObject(sound, "critical_db", 80);
    cJSON_AddNumberToObject(sound, "offset_db", 0);
    cJSON_AddBoolToObject(sound, "alarm", true);

    cJSON *light = cJSON_AddObjectToObject(root, "light");
    cJSON_AddBoolToObject(light, "enabled", true);
    cJSON_AddNumberToObject(light, "period_s", 5);
    cJSON_AddNumberToObject(light, "dark_lux", 50);
    cJSON_AddNumberToObject(light, "bright_lux", 1000);
    cJSON_AddNumberToObject(light, "offset_lux", 0);
    cJSON_AddBoolToObject(light, "alarm", true);

    return root;
}

static esp_err_t persist(void)
{
    if (s_sensors == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    char *json = cJSON_PrintUnformatted(s_sensors);
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

static void load(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        s_sensors = make_defaults();
        return;
    }

    size_t len = 0;
    esp_err_t err = nvs_get_str(handle, NVS_KEY_JSON, NULL, &len);
    if (err != ESP_OK || len == 0) {
        nvs_close(handle);
        s_sensors = make_defaults();
        return;
    }

    char *buf = malloc(len);
    if (buf == NULL) {
        nvs_close(handle);
        s_sensors = make_defaults();
        return;
    }
    err = nvs_get_str(handle, NVS_KEY_JSON, buf, &len);
    nvs_close(handle);
    if (err != ESP_OK) {
        free(buf);
        s_sensors = make_defaults();
        return;
    }

    s_sensors = cJSON_Parse(buf);
    free(buf);
    if (s_sensors == NULL || !cJSON_IsObject(s_sensors)) {
        cJSON_Delete(s_sensors);
        s_sensors = make_defaults();
        ESP_LOGW(TAG, "invalid NVS sensors json, using defaults");
    }
}

void sensor_settings_init(void)
{
    if (s_sensors != NULL) {
        return;
    }
    load();
    if (s_sensors == NULL) {
        s_sensors = make_defaults();
    }
}

cJSON *sensor_settings_to_json(void)
{
    sensor_settings_init();
    return cJSON_Duplicate(s_sensors, 1);
}

esp_err_t sensor_settings_from_json(const cJSON *sensors)
{
    if (sensors == NULL || !cJSON_IsObject(sensors)) {
        return ESP_ERR_INVALID_ARG;
    }
    sensor_settings_init();

    cJSON *dup = cJSON_Duplicate(sensors, 1);
    if (dup == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_Delete(s_sensors);
    s_sensors = dup;
    esp_err_t err = persist();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "persist sensors failed: %s", esp_err_to_name(err));
    }
    return err;
}

static const cJSON *sound_obj(void)
{
    sensor_settings_init();
    if (s_sensors == NULL) {
        return NULL;
    }
    return cJSON_GetObjectItem(s_sensors, "sound");
}

bool sensor_settings_sound_enabled(void)
{
    const cJSON *sound = sound_obj();
    const cJSON *en = sound ? cJSON_GetObjectItem(sound, "enabled") : NULL;
    if (cJSON_IsBool(en)) {
        return cJSON_IsTrue(en);
    }
    return true;
}

float sensor_settings_sound_offset_db(void)
{
    const cJSON *sound = sound_obj();
    const cJSON *off = sound ? cJSON_GetObjectItem(sound, "offset_db") : NULL;
    if (cJSON_IsNumber(off)) {
        return (float)off->valuedouble;
    }
    return 0.0f;
}

uint16_t sensor_settings_install_height_cm(void)
{
    sensor_settings_init();
    const cJSON *mm = s_sensors ? cJSON_GetObjectItem(s_sensors, "mmwave") : NULL;
    const cJSON *h = mm ? cJSON_GetObjectItem(mm, "install_height_m") : NULL;
    if (cJSON_IsNumber(h) && h->valuedouble > 0.0) {
        long cm = lround(h->valuedouble * 100.0);
        if (cm < 0) {
            cm = 0;
        }
        if (cm > 65535) {
            cm = 65535;
        }
        return (uint16_t)cm;
    }
    return (uint16_t)CONFIG_C4004_INSTALL_HEIGHT_CM;
}
