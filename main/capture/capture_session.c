/**
 * @file capture_session.c
 * @brief 实现 capture_session.h
 */
#include "capture_session.h"

#include "audio_capture.h"
#include "c4004_sensor.h"
#include "device_id.h"
#include "env_sensors.h"
#include "radar_area.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "sdkconfig.h"

#if CONFIG_ENV_SOUND_ENABLE
#include "env_sound.h"
#endif

static const char *TAG = "capture";

#define NVS_NS           "capture"
#define NVS_KEY_ROOM     "room"
#define PCM_RING_SAMPLES (AUDIO_CAPTURE_SAMPLE_RATE * 4)
#define TARGET_MAX       8

#define CROSSING_ENTER   0
#define CROSSING_LEAVE   1
#define CROSSING_NONE    2

typedef struct {
    bool valid;
    bool enter;
    bool exit;
    int people;
} zone_prev_t;

static SemaphoreHandle_t s_lock;
static char s_room[CAPTURE_ROOM_LEN];
static volatile bool s_active;
static volatile bool s_pcm_eof;
static time_t s_started_at;
static int s_sink_id = -1;
static zone_prev_t s_prev[AREA_TAG_MAX];

static int16_t *s_ring;
static size_t s_ring_w;
static size_t s_ring_r;
static size_t s_ring_count;
static SemaphoreHandle_t s_pcm_data;

static void ensure_lock(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
}

static void load_room_unlocked(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(s_room);
        if (nvs_get_str(h, NVS_KEY_ROOM, s_room, &len) != ESP_OK) {
            s_room[0] = '\0';
        }
        nvs_close(h);
    }
    if (s_room[0] == '\0') {
        (void)device_id_hostname(s_room, sizeof(s_room));
    }
}

static void persist_room_unlocked(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    (void)nvs_set_str(h, NVS_KEY_ROOM, s_room);
    (void)nvs_commit(h);
    nvs_close(h);
}

void capture_session_init(void)
{
    ensure_lock();
    if (s_pcm_data == NULL) {
        s_pcm_data = xSemaphoreCreateBinary();
    }
    if (s_lock != NULL && xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        load_room_unlocked();
        xSemaphoreGive(s_lock);
    }
    ESP_LOGI(TAG, "room=%s", s_room);
}

esp_err_t capture_session_set_room(const char *room)
{
    if (room == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ensure_lock();
    if (s_lock == NULL || xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    strncpy(s_room, room, sizeof(s_room) - 1);
    s_room[sizeof(s_room) - 1] = '\0';
    for (char *p = s_room; *p; p++) {
        if (*p == '\n' || *p == '\r') {
            *p = ' ';
        }
    }
    if (s_room[0] == '\0') {
        (void)device_id_hostname(s_room, sizeof(s_room));
    }
    persist_room_unlocked();
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

void capture_session_get_room(char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return;
    }
    out[0] = '\0';
    ensure_lock();
    if (s_lock != NULL && xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (s_room[0] == '\0') {
            load_room_unlocked();
        }
        strncpy(out, s_room, out_len - 1);
        out[out_len - 1] = '\0';
        xSemaphoreGive(s_lock);
    }
}

bool capture_session_time_ok(void)
{
    if (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
        return true;
    }
    return time(NULL) > 1700000000;
}

bool capture_session_is_active(void)
{
    return s_active;
}

time_t capture_session_started_at(void)
{
    return s_started_at;
}

int capture_session_remaining_s(void)
{
    if (!s_active) {
        return 0;
    }
    time_t now = time(NULL);
    int elapsed = (int)(now - s_started_at);
    if (elapsed < 0) {
        elapsed = 0;
    }
    int left = CAPTURE_SESSION_MAX_S - elapsed;
    return left > 0 ? left : 0;
}

static void on_pcm(const int16_t *pcm, size_t n_samples, void *ctx)
{
    (void)ctx;
    if (!s_active || s_ring == NULL || pcm == NULL || n_samples == 0) {
        return;
    }
    ensure_lock();
    if (s_lock == NULL || xSemaphoreTake(s_lock, pdMS_TO_TICKS(2)) != pdTRUE) {
        return;
    }
    for (size_t i = 0; i < n_samples; i++) {
        if (s_ring_count >= PCM_RING_SAMPLES) {
            s_ring_r = (s_ring_r + 1) % PCM_RING_SAMPLES;
            s_ring_count--;
        }
        s_ring[s_ring_w] = pcm[i];
        s_ring_w = (s_ring_w + 1) % PCM_RING_SAMPLES;
        s_ring_count++;
    }
    xSemaphoreGive(s_lock);
    if (s_pcm_data != NULL) {
        xSemaphoreGive(s_pcm_data);
    }
}

static void tear_down_unlocked(void)
{
    s_active = false;
    s_pcm_eof = true;
    if (s_sink_id >= 0) {
        audio_capture_remove_sink(s_sink_id);
        s_sink_id = -1;
        audio_capture_release();
    }
    if (s_pcm_data != NULL) {
        xSemaphoreGive(s_pcm_data);
    }
}

esp_err_t capture_session_start(void)
{
    if (!capture_session_time_ok()) {
        return ESP_ERR_INVALID_STATE;
    }
    ensure_lock();
    if (s_lock == NULL || xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_active) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_room[0] == '\0') {
        load_room_unlocked();
    }
    if (s_ring == NULL) {
        s_ring = heap_caps_malloc(PCM_RING_SAMPLES * sizeof(int16_t), MALLOC_CAP_8BIT);
        if (s_ring == NULL) {
            s_ring = malloc(PCM_RING_SAMPLES * sizeof(int16_t));
        }
    }
    if (s_ring == NULL) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NO_MEM;
    }
    s_ring_w = s_ring_r = s_ring_count = 0;
    memset(s_prev, 0, sizeof(s_prev));
    s_pcm_eof = false;
    xSemaphoreGive(s_lock);

    esp_err_t err = audio_capture_acquire();
    if (err != ESP_OK) {
        return err;
    }
    err = audio_capture_add_sink(on_pcm, NULL, &s_sink_id);
    if (err != ESP_OK) {
        audio_capture_release();
        s_sink_id = -1;
        return err;
    }

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) == pdTRUE) {
        s_started_at = time(NULL);
        s_active = true;
        xSemaphoreGive(s_lock);
    }
    ESP_LOGI(TAG, "start room=%s t=%ld", s_room, (long)s_started_at);
    return ESP_OK;
}

void capture_session_stop(void)
{
    ensure_lock();
    if (s_lock != NULL && xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) == pdTRUE) {
        tear_down_unlocked();
        xSemaphoreGive(s_lock);
    }
    ESP_LOGI(TAG, "stop");
}

void capture_session_abort(void)
{
    capture_session_stop();
    ESP_LOGW(TAG, "aborted");
}

int capture_session_read_pcm(void *buf, size_t buf_len, uint32_t wait_ms)
{
    if (buf == NULL || buf_len < sizeof(int16_t)) {
        return 0;
    }
    size_t max_samp = buf_len / sizeof(int16_t);
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(wait_ms);

    while (true) {
        ensure_lock();
        if (s_lock != NULL && xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
            if (s_ring_count > 0 && s_ring != NULL) {
                size_t n = s_ring_count < max_samp ? s_ring_count : max_samp;
                int16_t *out = (int16_t *)buf;
                for (size_t i = 0; i < n; i++) {
                    out[i] = s_ring[s_ring_r];
                    s_ring_r = (s_ring_r + 1) % PCM_RING_SAMPLES;
                }
                s_ring_count -= n;
                xSemaphoreGive(s_lock);
                return (int)(n * sizeof(int16_t));
            }
            bool eof = s_pcm_eof && s_ring_count == 0;
            xSemaphoreGive(s_lock);
            if (eof) {
                return 0;
            }
        }
        if ((int32_t)(deadline - xTaskGetTickCount()) <= 0) {
            return -1;
        }
        if (s_pcm_data != NULL) {
            xSemaphoreTake(s_pcm_data, pdMS_TO_TICKS(20));
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

static void expire_if_needed(void)
{
    if (s_active && capture_session_remaining_s() <= 0) {
        capture_session_stop();
    }
}

static int kinesia_bucket(uint8_t kinesia)
{
    if (kinesia == 0) {
        return -1;
    }
    if (kinesia == 1) {
        return 0;
    }
    if (kinesia <= 30) {
        return 1;
    }
    if (kinesia <= 60) {
        return 2;
    }
    return 3;
}

static bool point_in_rect(const area_rect_t *rect, int16_t x, int16_t y)
{
    return x >= (int16_t)rect->min_x && x <= (int16_t)rect->max_x &&
           y >= (int16_t)rect->min_y && y <= (int16_t)rect->max_y;
}

cJSON *capture_session_status_json(void)
{
    expire_if_needed();
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }
    char room[CAPTURE_ROOM_LEN];
    capture_session_get_room(room, sizeof(room));
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "time_ok", capture_session_time_ok());
    cJSON_AddBoolToObject(root, "recording", s_active);
    cJSON_AddStringToObject(root, "room", room);
    cJSON_AddNumberToObject(root, "sample_rate", AUDIO_CAPTURE_SAMPLE_RATE);
    cJSON_AddNumberToObject(root, "max_s", CAPTURE_SESSION_MAX_S);
    cJSON_AddNumberToObject(root, "remaining_s", capture_session_remaining_s());
    cJSON_AddNumberToObject(root, "started_at", (double)s_started_at);
    return root;
}

cJSON *capture_session_sample_json(void)
{
    expire_if_needed();
    if (!s_active) {
        return NULL;
    }

    (void)c4004_sensor_refresh_targets();

    env_snapshot_t env = {0};
    (void)env_sensors_get(&env);

    float sound_db = 0.0f;
    bool sound_ok = false;
#if CONFIG_ENV_SOUND_ENABLE
    sound_ok = env_sound_get_db(&sound_db);
#endif

    area_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    (void)area_config_get(&cfg);

    area_status_t areas[AREA_TAG_MAX];
    uint8_t area_count = 0;
    (void)radar_area_get_status(areas, &area_count);

    c4004_target_info_t targets[TARGET_MAX];
    uint8_t n_targets = 0;
    (void)c4004_sensor_get_targets(targets, TARGET_MAX, &n_targets);

    char room[CAPTURE_ROOM_LEN];
    capture_session_get_room(room, sizeof(room));

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddNumberToObject(root, "timestamp", (double)time(NULL));
    cJSON_AddNumberToObject(root, "remaining_s", capture_session_remaining_s());
    cJSON_AddStringToObject(root, "room", room);

    cJSON *envj = cJSON_AddObjectToObject(root, "env");
    if (env.env_ready) {
        cJSON_AddNumberToObject(envj, "temperature_c", env.temperature);
        cJSON_AddNumberToObject(envj, "humidity_pct", env.humidity);
        cJSON_AddNumberToObject(envj, "co2_ppm", env.co2);
    } else {
        cJSON_AddNullToObject(envj, "temperature_c");
        cJSON_AddNullToObject(envj, "humidity_pct");
        cJSON_AddNullToObject(envj, "co2_ppm");
    }
    if (env.lux_ready) {
        cJSON_AddNumberToObject(envj, "lux", env.lux);
    } else {
        cJSON_AddNullToObject(envj, "lux");
    }
    if (sound_ok) {
        cJSON_AddNumberToObject(envj, "sound_db", sound_db);
    } else {
        cJSON_AddNullToObject(envj, "sound_db");
    }

    cJSON *radar = cJSON_AddArrayToObject(root, "radar");
    uint8_t n = cfg.tag_count < area_count ? cfg.tag_count : area_count;
    if (n > AREA_TAG_MAX) {
        n = AREA_TAG_MAX;
    }

    for (uint8_t i = 0; i < n; i++) {
        uint8_t strategy = cfg.tags[i].strategy;
        if (strategy != 1 && strategy != 2) {
            continue;
        }
        area_rect_t rect;
        if (area_coords_to_rect(&cfg.tags[i], &rect) != ESP_OK) {
            continue;
        }

        int buckets[4] = {0, 0, 0, 0};
        for (uint8_t t = 0; t < n_targets; t++) {
            if (!point_in_rect(&rect, targets[t].x_cm, targets[t].y_cm)) {
                continue;
            }
            if (area_point_in_noise_zone((float)targets[t].x_cm, (float)targets[t].y_cm)) {
                continue;
            }
            int b = kinesia_bucket(targets[t].kinesia);
            if (b >= 0) {
                buckets[b]++;
            }
        }
        int people = buckets[0] + buckets[1] + buckets[2] + buckets[3];

        int crossing = CROSSING_NONE;
        zone_prev_t *prev = &s_prev[i];
        if (strategy == 2) {
            bool enter = areas[i].enter;
            bool exit = areas[i].exit;
            if (prev->valid) {
                if (enter && !prev->enter) {
                    crossing = CROSSING_ENTER;
                }
                if (exit && !prev->exit) {
                    crossing = CROSSING_LEAVE;
                }
            }
            prev->enter = enter;
            prev->exit = exit;
        } else {
            if (prev->valid) {
                if (prev->people == 0 && people > 0) {
                    crossing = CROSSING_ENTER;
                } else if (prev->people > 0 && people == 0) {
                    crossing = CROSSING_LEAVE;
                }
            }
        }
        prev->people = people;
        prev->valid = true;

        const char *zname = cfg.tags[i].name[0] ? cfg.tags[i].name : areas[i].name;
        cJSON *row = cJSON_CreateObject();
        cJSON_AddStringToObject(row, "zone", zname);
        cJSON_AddNumberToObject(row, "stationary", buckets[0]);
        cJSON_AddNumberToObject(row, "micro", buckets[1]);
        cJSON_AddNumberToObject(row, "slow", buckets[2]);
        cJSON_AddNumberToObject(row, "fast", buckets[3]);
        cJSON_AddNumberToObject(row, "crossing", crossing);
        cJSON_AddItemToArray(radar, row);
    }

    return root;
}
