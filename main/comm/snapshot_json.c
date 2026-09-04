/**
 * @file snapshot_json.c
 * @brief 字段名是线上契约。HA sensor.py / 脚本 get_snapshot 按这些键读，改名须三方同步。
 */
#include "snapshot_json.h"

#include "board_snapshot.h"
#include "sensor_settings.h"
#include "sdkconfig.h"

#if CONFIG_ENV_SOUND_ENABLE
#include "env_sound.h"
#endif

#include <math.h>
#include <string.h>

static double round1(double v)
{
    return round(v * 10.0) / 10.0;
}

cJSON *snapshot_json_build(void)
{
    /* 静态：避免在 coap_hub 任务栈上再叠 ~1KB sensor_snapshot_t */
    static sensor_snapshot_t snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    if (board_snapshot_get(&snapshot) != ESP_OK) {
        return NULL;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }

    cJSON_AddBoolToObject(root, "env_ready", snapshot.env_ready);
    if (snapshot.env_ready) {
        cJSON_AddNumberToObject(root, "temperature_c", round1(snapshot.temperature));
        cJSON_AddNumberToObject(root, "humidity_pct", round1(snapshot.humidity));
        cJSON_AddNumberToObject(root, "co2_ppm", snapshot.co2);
    } else {
        cJSON_AddNullToObject(root, "temperature_c");
        cJSON_AddNullToObject(root, "humidity_pct");
        cJSON_AddNullToObject(root, "co2_ppm");
    }

    cJSON_AddBoolToObject(root, "lux_ready", snapshot.lux_ready);
    if (snapshot.lux_ready) {
        cJSON_AddNumberToObject(root, "lux", (int)lround(snapshot.lux));
    } else {
        cJSON_AddNullToObject(root, "lux");
    }

    cJSON *areas = cJSON_AddArrayToObject(root, "areas");
    for (uint8_t i = 0; i < snapshot.area_count; i++) {
        const area_status_t *st = &snapshot.areas[i];
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

    cJSON *thresholds = sensor_settings_to_json();
    if (thresholds != NULL) {
        cJSON_AddItemToObject(root, "thresholds", thresholds);
    }

    return root;
}
