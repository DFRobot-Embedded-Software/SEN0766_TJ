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

static void set_number(cJSON *obj, const char *key, double value)
{
    if (obj == NULL || key == NULL) {
        return;
    }
    cJSON_DeleteItemFromObject(obj, key);
    cJSON_AddNumberToObject(obj, key, value);
}

static cJSON *ensure_object(cJSON *root, const char *key)
{
    cJSON *obj = cJSON_GetObjectItem(root, key);
    if (cJSON_IsObject(obj)) {
        return obj;
    }
    cJSON_DeleteItemFromObject(root, key);
    return cJSON_AddObjectToObject(root, key);
}

static cJSON *make_defaults(void)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }

    /* 安装高度仅固件内部使用，不在网页阈值页展示 */
    cJSON *mm = cJSON_AddObjectToObject(root, "mmwave");
    cJSON_AddNumberToObject(mm, "install_height_m", 1.8);

    cJSON *th = cJSON_AddObjectToObject(root, "temperature_humidity");
    cJSON_AddNumberToObject(th, "temp_high", 30);
    cJSON_AddNumberToObject(th, "humidity_high", 80);
    cJSON_AddNumberToObject(th, "humidity_low", 20);

    cJSON *co2 = cJSON_AddObjectToObject(root, "co2");
    cJSON_AddNumberToObject(co2, "warning_ppm", 1000);

    cJSON *sound = cJSON_AddObjectToObject(root, "sound");
    cJSON_AddBoolToObject(sound, "enabled", true);
    cJSON_AddNumberToObject(sound, "warning_db", 60);
    cJSON_AddNumberToObject(sound, "offset_db", 0);

    cJSON *light = cJSON_AddObjectToObject(root, "light");
    cJSON_AddNumberToObject(light, "dark_lux", 50);
    cJSON_AddNumberToObject(light, "bright_lux", 1000);

    return root;
}

static void merge_number_field(cJSON *dst, const cJSON *src, const char *key)
{
    if (dst == NULL || src == NULL || key == NULL) {
        return;
    }
    const cJSON *v = cJSON_GetObjectItem(src, key);
    if (cJSON_IsNumber(v)) {
        set_number(dst, key, v->valuedouble);
    }
}

static void normalize(cJSON *root)
{
    if (!cJSON_IsObject(root)) {
        return;
    }

    cJSON *mm = ensure_object(root, "mmwave");
    if (cJSON_GetObjectItem(mm, "install_height_m") == NULL) {
        cJSON_AddNumberToObject(mm, "install_height_m", 1.8);
    }

    cJSON *th = ensure_object(root, "temperature_humidity");
    if (cJSON_GetObjectItem(th, "temp_high") == NULL) {
        cJSON_AddNumberToObject(th, "temp_high", 30);
    }
    if (cJSON_GetObjectItem(th, "humidity_high") == NULL) {
        cJSON_AddNumberToObject(th, "humidity_high", 80);
    }
    if (cJSON_GetObjectItem(th, "humidity_low") == NULL) {
        cJSON_AddNumberToObject(th, "humidity_low", 20);
    }

    cJSON *co2 = ensure_object(root, "co2");
    if (cJSON_GetObjectItem(co2, "warning_ppm") == NULL) {
        cJSON_AddNumberToObject(co2, "warning_ppm", 1000);
    }

    cJSON *sound = ensure_object(root, "sound");
    if (cJSON_GetObjectItem(sound, "enabled") == NULL) {
        cJSON_AddBoolToObject(sound, "enabled", true);
    }
    if (cJSON_GetObjectItem(sound, "warning_db") == NULL) {
        cJSON_AddNumberToObject(sound, "warning_db", 60);
    }
    if (cJSON_GetObjectItem(sound, "offset_db") == NULL) {
        cJSON_AddNumberToObject(sound, "offset_db", 0);
    }

    cJSON *light = ensure_object(root, "light");
    if (cJSON_GetObjectItem(light, "dark_lux") == NULL) {
        cJSON_AddNumberToObject(light, "dark_lux", 50);
    }
    if (cJSON_GetObjectItem(light, "bright_lux") == NULL) {
        cJSON_AddNumberToObject(light, "bright_lux", 1000);
    }
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
        return;
    }
    normalize(s_sensors);
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
    /* 网页仅需要异常判定阈值；安装高度仍保留在内部 NVS */
    cJSON *out = cJSON_CreateObject();
    if (out == NULL) {
        return NULL;
    }
    const cJSON *th = cJSON_GetObjectItem(s_sensors, "temperature_humidity");
    cJSON *th_out = cJSON_AddObjectToObject(out, "temperature_humidity");
    if (cJSON_IsObject(th) && th_out) {
        const cJSON *v;
        v = cJSON_GetObjectItem(th, "temp_high");
        cJSON_AddNumberToObject(th_out, "temp_high", cJSON_IsNumber(v) ? v->valuedouble : 30);
        v = cJSON_GetObjectItem(th, "humidity_high");
        cJSON_AddNumberToObject(th_out, "humidity_high", cJSON_IsNumber(v) ? v->valuedouble : 80);
        v = cJSON_GetObjectItem(th, "humidity_low");
        cJSON_AddNumberToObject(th_out, "humidity_low", cJSON_IsNumber(v) ? v->valuedouble : 20);
    }

    const cJSON *co2 = cJSON_GetObjectItem(s_sensors, "co2");
    cJSON *co2_out = cJSON_AddObjectToObject(out, "co2");
    if (co2_out) {
        const cJSON *v = cJSON_IsObject(co2) ? cJSON_GetObjectItem(co2, "warning_ppm") : NULL;
        cJSON_AddNumberToObject(co2_out, "warning_ppm", cJSON_IsNumber(v) ? v->valuedouble : 1000);
    }

    const cJSON *sound = cJSON_GetObjectItem(s_sensors, "sound");
    cJSON *sound_out = cJSON_AddObjectToObject(out, "sound");
    if (sound_out) {
        const cJSON *v = cJSON_IsObject(sound) ? cJSON_GetObjectItem(sound, "warning_db") : NULL;
        cJSON_AddNumberToObject(sound_out, "warning_db", cJSON_IsNumber(v) ? v->valuedouble : 60);
    }

    const cJSON *light = cJSON_GetObjectItem(s_sensors, "light");
    cJSON *light_out = cJSON_AddObjectToObject(out, "light");
    if (light_out) {
        const cJSON *v;
        v = cJSON_IsObject(light) ? cJSON_GetObjectItem(light, "dark_lux") : NULL;
        cJSON_AddNumberToObject(light_out, "dark_lux", cJSON_IsNumber(v) ? v->valuedouble : 50);
        v = cJSON_IsObject(light) ? cJSON_GetObjectItem(light, "bright_lux") : NULL;
        cJSON_AddNumberToObject(light_out, "bright_lux", cJSON_IsNumber(v) ? v->valuedouble : 1000);
    }
    return out;
}

esp_err_t sensor_settings_from_json(const cJSON *sensors)
{
    if (sensors == NULL || !cJSON_IsObject(sensors)) {
        return ESP_ERR_INVALID_ARG;
    }
    sensor_settings_init();
    normalize(s_sensors);

    const cJSON *th = cJSON_GetObjectItem(sensors, "temperature_humidity");
    if (cJSON_IsObject(th)) {
        cJSON *dst = ensure_object(s_sensors, "temperature_humidity");
        merge_number_field(dst, th, "temp_high");
        merge_number_field(dst, th, "humidity_high");
        merge_number_field(dst, th, "humidity_low");
    }

    const cJSON *co2 = cJSON_GetObjectItem(sensors, "co2");
    if (cJSON_IsObject(co2)) {
        merge_number_field(ensure_object(s_sensors, "co2"), co2, "warning_ppm");
    }

    const cJSON *sound = cJSON_GetObjectItem(sensors, "sound");
    if (cJSON_IsObject(sound)) {
        merge_number_field(ensure_object(s_sensors, "sound"), sound, "warning_db");
    }

    const cJSON *light = cJSON_GetObjectItem(sensors, "light");
    if (cJSON_IsObject(light)) {
        cJSON *dst = ensure_object(s_sensors, "light");
        merge_number_field(dst, light, "dark_lux");
        merge_number_field(dst, light, "bright_lux");
    }

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
