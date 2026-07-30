/**
 * @file env_event_detector.c
 * @brief 环境阈值检测：持续越限确认后边沿 emit
 */

#include "env_event_detector.h"

#include <stdio.h>
#include <string.h>

#include "board_snapshot.h"
#include "cJSON.h"
#include "device_event_bus.h"
#include "device_event_types.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "sensor_settings.h"

#if CONFIG_ENV_SOUND_ENABLE
#include "env_sound.h"
#endif

static const char *TAG = "env_evt";

#ifndef CONFIG_EVENT_ENV_POLL_MS
#define CONFIG_EVENT_ENV_POLL_MS 2000
#endif
#ifndef CONFIG_EVENT_ENV_CONFIRM_MS
#define CONFIG_EVENT_ENV_CONFIRM_MS 60000
#endif

#define ENV_WATCH_STACK 6144
#define MSG_LEN 160

typedef enum {
    ENV_CH_TEMP_HIGH = 0,
    ENV_CH_HUM_HIGH,
    ENV_CH_HUM_LOW,
    ENV_CH_CO2,
    ENV_CH_LUX_DARK,
    ENV_CH_LUX_BRIGHT,
    ENV_CH_SOUND_DB,
    ENV_CH_COUNT
} env_channel_t;

typedef struct {
    bool over;
    bool fired;
    TickType_t since;
} env_track_t;

static TaskHandle_t s_task;
static bool s_running;
static env_track_t s_track[ENV_CH_COUNT];

static bool recovered_high(float value, float thr)
{
    /* 5% 回差：低于 thr * 0.95 才算恢复 */
    return value < thr * 0.95f;
}

static bool recovered_low(float value, float thr)
{
    return value > thr * 1.05f;
}

static void emit_env(const char *type, const char *category, const char *message,
                     float value, float threshold)
{
    cJSON *payload = cJSON_CreateObject();
    if (payload != NULL) {
        cJSON_AddNumberToObject(payload, "value", value);
        cJSON_AddNumberToObject(payload, "threshold", threshold);
    }

    device_event_t ev = {
        .type = type,
        .category = category,
        .area_id = DEVICE_EVENT_AREA_ID_GLOBAL,
        .area = NULL,
        .person_id = -1,
        .active = -1,
        .static_count = -1,
        .message = message,
        .payload = payload,
    };
    (void)device_event_emit(&ev);
    cJSON_Delete(payload);
}

static void update_channel(env_channel_t ch,
                           bool active,
                           bool over,
                           TickType_t now,
                           TickType_t confirm,
                           const char *type,
                           const char *category,
                           const char *message,
                           float value,
                           float threshold,
                           bool (*recovered)(float, float))
{
    env_track_t *t = &s_track[ch];
    if (!active) {
        t->over = false;
        t->since = 0;
        return;
    }

    if (over) {
        if (!t->over) {
            t->over = true;
            t->since = now;
        }
        if (!t->fired && t->since != 0 && (now - t->since) >= confirm) {
            emit_env(type, category, message, value, threshold);
            t->fired = true;
        }
    } else {
        bool clear = true;
        if (t->fired && recovered != NULL) {
            clear = recovered(value, threshold);
        }
        if (clear) {
            t->over = false;
            t->since = 0;
            t->fired = false;
        }
    }
}

static void poll_once(TickType_t now, TickType_t confirm)
{
    sensor_snapshot_t snap = {0};
    if (board_snapshot_get(&snap) != ESP_OK) {
        return;
    }

    char msg[MSG_LEN];

    if (snap.env_ready) {
        const float th_temp = sensor_settings_temp_high();
        snprintf(msg, sizeof(msg), "温度超过阈值 %.0f°C，当前 %.1f°C",
                 (double)th_temp, (double)snap.temperature);
        update_channel(ENV_CH_TEMP_HIGH, true, snap.temperature >= th_temp, now, confirm,
                       DEVICE_EVENT_TEMP_HIGH, DEVICE_EVENT_CAT_ENV, msg,
                       snap.temperature, th_temp, recovered_high);

        const float th_hh = sensor_settings_humidity_high();
        snprintf(msg, sizeof(msg), "湿度超过阈值 %.0f%%，当前 %.1f%%",
                 (double)th_hh, (double)snap.humidity);
        update_channel(ENV_CH_HUM_HIGH, true, snap.humidity >= th_hh, now, confirm,
                       DEVICE_EVENT_HUMIDITY_HIGH, DEVICE_EVENT_CAT_ENV, msg,
                       snap.humidity, th_hh, recovered_high);

        const float th_hl = sensor_settings_humidity_low();
        snprintf(msg, sizeof(msg), "湿度低于阈值 %.0f%%，当前 %.1f%%",
                 (double)th_hl, (double)snap.humidity);
        update_channel(ENV_CH_HUM_LOW, true, snap.humidity <= th_hl, now, confirm,
                       DEVICE_EVENT_HUMIDITY_LOW, DEVICE_EVENT_CAT_ENV, msg,
                       snap.humidity, th_hl, recovered_low);

        const float th_co2 = (float)sensor_settings_co2_warning_ppm();
        snprintf(msg, sizeof(msg), "CO2 超过阈值 %.0fppm，当前 %dppm",
                 (double)th_co2, snap.co2);
        update_channel(ENV_CH_CO2, true, (float)snap.co2 >= th_co2, now, confirm,
                       DEVICE_EVENT_CO2_HIGH, DEVICE_EVENT_CAT_ENV, msg,
                       (float)snap.co2, th_co2, recovered_high);
    }

    if (snap.lux_ready) {
        const float th_dark = sensor_settings_lux_dark();
        snprintf(msg, sizeof(msg), "光照过暗（≤%.0flux），当前 %.0flux",
                 (double)th_dark, (double)snap.lux);
        update_channel(ENV_CH_LUX_DARK, true, snap.lux <= th_dark, now, confirm,
                       DEVICE_EVENT_LUX_DARK, DEVICE_EVENT_CAT_ENV, msg,
                       snap.lux, th_dark, recovered_low);

        const float th_bright = sensor_settings_lux_bright();
        snprintf(msg, sizeof(msg), "光照过亮（≥%.0flux），当前 %.0flux",
                 (double)th_bright, (double)snap.lux);
        update_channel(ENV_CH_LUX_BRIGHT, true, snap.lux >= th_bright, now, confirm,
                       DEVICE_EVENT_LUX_BRIGHT, DEVICE_EVENT_CAT_ENV, msg,
                       snap.lux, th_bright, recovered_high);
    }

#if CONFIG_ENV_SOUND_ENABLE
    if (sensor_settings_sound_enabled()) {
        float db = 0.0f;
        if (env_sound_get_db(&db)) {
            db += sensor_settings_sound_offset_db();
            const float th_db = sensor_settings_sound_warning_db();
            snprintf(msg, sizeof(msg), "环境声级超过阈值 %.0fdB，当前 %.0fdB",
                     (double)th_db, (double)db);
            update_channel(ENV_CH_SOUND_DB, true, db >= th_db, now, confirm,
                           DEVICE_EVENT_SOUND_DB_HIGH, DEVICE_EVENT_CAT_SOUND, msg,
                           db, th_db, recovered_high);
        }
    }
#endif
}

static void env_watch_task(void *arg)
{
    (void)arg;
    const TickType_t interval = pdMS_TO_TICKS(CONFIG_EVENT_ENV_POLL_MS);
    const TickType_t confirm = pdMS_TO_TICKS(CONFIG_EVENT_ENV_CONFIRM_MS);

    while (s_running) {
        poll_once(xTaskGetTickCount(), confirm);
        vTaskDelay(interval);
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t env_event_detector_start(void)
{
    if (s_running) {
        return ESP_OK;
    }
    esp_err_t err = device_event_bus_init();
    if (err != ESP_OK) {
        return err;
    }
    memset(s_track, 0, sizeof(s_track));
    s_running = true;
    if (xTaskCreate(env_watch_task, "env_evt", ENV_WATCH_STACK, NULL, 3, &s_task) != pdPASS) {
        s_running = false;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "env event detector started (poll=%dms confirm=%dms)",
             CONFIG_EVENT_ENV_POLL_MS, CONFIG_EVENT_ENV_CONFIRM_MS);
    return ESP_OK;
}

void env_event_detector_stop(void)
{
    if (!s_running) {
        return;
    }
    s_running = false;
    while (s_task != NULL) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
