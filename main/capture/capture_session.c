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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdkconfig.h"

#if CONFIG_ENV_SOUND_ENABLE
#include "env_sound.h"
#endif

static const char *TAG = "capture";

#define NVS_NS           "capture"
#define NVS_KEY_ROOM     "room"
#define NVS_KEY_DUR      "dur_s"
#define NVS_KEY_EVENT    "event"
#define NVS_KEY_SEQ      "seq"
#define SAMPLE_ID_MAX    999999
#define PCM_RING_SAMPLES (AUDIO_CAPTURE_SAMPLE_RATE * 4)
#define TARGET_MAX       8
#define ENV_LOG_CAP      (48 * 1024)
#define RADAR_LOG_CAP    (256 * 1024)

#define ENV_CSV_HDR   "timestamp,temperature_c,humidity_pct,co2_ppm,lux,sound_db\n"
#define RADAR_CSV_HDR "timestamp,room,zone,stationary,micro,slow,fast,crossing\n"

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
static char s_event[CAPTURE_EVENT_LEN];
static int s_duration_s = CAPTURE_SESSION_MAX_S;
static int32_t s_seq;
static int s_run_s;
static char s_run_event[CAPTURE_EVENT_LEN];
static char s_run_start_iso[32];
static char s_last_sample_id[16];
static char s_last_event[CAPTURE_EVENT_LEN];
static char s_last_start_iso[32];
static bool s_package_valid;
static volatile bool s_time_synced;
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
static char *s_env_log;
static char *s_radar_log;
static size_t s_env_len;
static size_t s_radar_len;
static TickType_t s_last_sample_tick;
static TaskHandle_t s_tick_task;

static void capture_record_sample(void);
static void capture_tick_task(void *arg);
static void expire_if_needed(void);

static void ensure_lock(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
}

static bool duration_in_range(int seconds)
{
    return seconds >= CAPTURE_SESSION_MIN_S && seconds <= CAPTURE_SESSION_MAX_S;
}

static void sanitize_text(char *s)
{
    if (s == NULL) {
        return;
    }
    for (char *p = s; *p; p++) {
        if (*p == '\n' || *p == '\r') {
            *p = ' ';
        }
    }
}

static void format_sample_id(int32_t seq, char *out, size_t out_len)
{
    snprintf(out, out_len, "sample_%06ld", (long)seq);
}

static void format_iso_local(time_t t, char *buf, size_t len)
{
    struct tm tm_local;
    if (t <= 0 || localtime_r(&t, &tm_local) == NULL) {
        snprintf(buf, len, "1970-01-01T00:00:00.000");
        return;
    }
    strftime(buf, len, "%Y-%m-%dT%H:%M:%S.000", &tm_local);
}

static void load_nvs_unlocked(void)
{
    nvs_handle_t h;
    int32_t dur = CAPTURE_SESSION_MAX_S;
    int32_t seq = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(s_room);
        if (nvs_get_str(h, NVS_KEY_ROOM, s_room, &len) != ESP_OK) {
            s_room[0] = '\0';
        }
        len = sizeof(s_event);
        if (nvs_get_str(h, NVS_KEY_EVENT, s_event, &len) != ESP_OK) {
            s_event[0] = '\0';
        }
        if (nvs_get_i32(h, NVS_KEY_DUR, &dur) != ESP_OK) {
            dur = CAPTURE_SESSION_MAX_S;
        }
        if (nvs_get_i32(h, NVS_KEY_SEQ, &seq) != ESP_OK) {
            seq = 0;
        }
        nvs_close(h);
    } else {
        s_room[0] = '\0';
        s_event[0] = '\0';
    }
    if (s_room[0] == '\0') {
        (void)device_id_hostname(s_room, sizeof(s_room));
    }
    s_duration_s = duration_in_range((int)dur) ? (int)dur : CAPTURE_SESSION_MAX_S;
    if (seq < 0) {
        seq = 0;
    }
    if (seq > SAMPLE_ID_MAX) {
        seq = SAMPLE_ID_MAX;
    }
    s_seq = seq;
}

static void persist_nvs_unlocked(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    (void)nvs_set_str(h, NVS_KEY_ROOM, s_room);
    (void)nvs_set_str(h, NVS_KEY_EVENT, s_event);
    (void)nvs_set_i32(h, NVS_KEY_DUR, s_duration_s);
    (void)nvs_set_i32(h, NVS_KEY_SEQ, s_seq);
    (void)nvs_commit(h);
    nvs_close(h);
}

static bool buf_add(char *buf, size_t *len, size_t cap, const char *s)
{
    if (buf == NULL || s == NULL) {
        return false;
    }
    size_t n = strlen(s);
    if (*len + n + 1 > cap) {
        return false;
    }
    memcpy(buf + *len, s, n);
    *len += n;
    buf[*len] = '\0';
    return true;
}

static void csv_quote(char *dst, size_t dst_len, const char *s)
{
    if (dst == NULL || dst_len == 0) {
        return;
    }
    if (s == NULL) {
        s = "";
    }
    bool quote = false;
    for (const char *p = s; *p; p++) {
        if (*p == ',' || *p == '"' || *p == '\n' || *p == '\r') {
            quote = true;
            break;
        }
    }
    if (!quote) {
        snprintf(dst, dst_len, "%s", s);
        return;
    }
    size_t o = 0;
    if (o + 1 < dst_len) {
        dst[o++] = '"';
    }
    for (; *s && o + 2 < dst_len; s++) {
        if (*s == '"') {
            dst[o++] = '"';
            if (o + 1 < dst_len) {
                dst[o++] = '"';
            }
        } else {
            dst[o++] = *s;
        }
    }
    if (o + 1 < dst_len) {
        dst[o++] = '"';
    }
    dst[o] = '\0';
}

static void ensure_logs_unlocked(void)
{
    if (s_env_log == NULL) {
        s_env_log = heap_caps_malloc(ENV_LOG_CAP, MALLOC_CAP_8BIT);
        if (s_env_log == NULL) {
            s_env_log = malloc(ENV_LOG_CAP);
        }
    }
    if (s_radar_log == NULL) {
        s_radar_log = heap_caps_malloc(RADAR_LOG_CAP, MALLOC_CAP_8BIT);
        if (s_radar_log == NULL) {
            s_radar_log = malloc(RADAR_LOG_CAP);
        }
    }
}

static void reset_logs_unlocked(void)
{
    ensure_logs_unlocked();
    s_env_len = 0;
    s_radar_len = 0;
    if (s_env_log) {
        s_env_log[0] = '\0';
        (void)buf_add(s_env_log, &s_env_len, ENV_LOG_CAP, ENV_CSV_HDR);
    }
    if (s_radar_log) {
        s_radar_log[0] = '\0';
        (void)buf_add(s_radar_log, &s_radar_len, RADAR_LOG_CAP, RADAR_CSV_HDR);
    }
}

static void clear_logs_unlocked(void)
{
    s_env_len = 0;
    s_radar_len = 0;
    if (s_env_log) {
        s_env_log[0] = '\0';
    }
    if (s_radar_log) {
        s_radar_log[0] = '\0';
    }
}

void capture_session_init(void)
{
    ensure_lock();
    if (s_pcm_data == NULL) {
        s_pcm_data = xSemaphoreCreateBinary();
    }
    if (s_lock != NULL && xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        load_nvs_unlocked();
        xSemaphoreGive(s_lock);
    }
    if (s_tick_task == NULL) {
        (void)xTaskCreate(capture_tick_task, "cap_tick", 12288, NULL, 4, &s_tick_task);
    }
    ESP_LOGI(TAG, "room=%s event=%s duration_s=%d seq=%ld", s_room, s_event, s_duration_s,
             (long)s_seq);
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
    sanitize_text(s_room);
    if (s_room[0] == '\0') {
        (void)device_id_hostname(s_room, sizeof(s_room));
    }
    persist_nvs_unlocked();
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
            load_nvs_unlocked();
        }
        strncpy(out, s_room, out_len - 1);
        out[out_len - 1] = '\0';
        xSemaphoreGive(s_lock);
    }
}

esp_err_t capture_session_set_duration_s(int seconds)
{
    if (!duration_in_range(seconds)) {
        return ESP_ERR_INVALID_ARG;
    }
    ensure_lock();
    if (s_lock == NULL || xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    s_duration_s = seconds;
    persist_nvs_unlocked();
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

int capture_session_get_duration_s(void)
{
    int v = CAPTURE_SESSION_MAX_S;
    ensure_lock();
    if (s_lock != NULL && xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        v = s_duration_s;
        xSemaphoreGive(s_lock);
    }
    return v;
}

esp_err_t capture_session_set_event(const char *event)
{
    if (event == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ensure_lock();
    if (s_lock == NULL || xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    strncpy(s_event, event, sizeof(s_event) - 1);
    s_event[sizeof(s_event) - 1] = '\0';
    sanitize_text(s_event);
    char *p = s_event;
    while (*p == ' ') {
        p++;
    }
    if (p != s_event) {
        memmove(s_event, p, strlen(p) + 1);
    }
    size_t n = strlen(s_event);
    while (n > 0 && s_event[n - 1] == ' ') {
        s_event[--n] = '\0';
    }
    persist_nvs_unlocked();
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

void capture_session_get_event(char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return;
    }
    out[0] = '\0';
    ensure_lock();
    if (s_lock != NULL && xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        strncpy(out, s_event, out_len - 1);
        out[out_len - 1] = '\0';
        xSemaphoreGive(s_lock);
    }
}

static void commit_sample_unlocked(void)
{
    if (s_seq >= SAMPLE_ID_MAX) {
        s_seq = 0;
    }
    s_seq++;
    format_sample_id(s_seq, s_last_sample_id, sizeof(s_last_sample_id));
    strncpy(s_last_event, s_run_event, sizeof(s_last_event) - 1);
    s_last_event[sizeof(s_last_event) - 1] = '\0';
    strncpy(s_last_start_iso, s_run_start_iso, sizeof(s_last_start_iso) - 1);
    s_last_start_iso[sizeof(s_last_start_iso) - 1] = '\0';
    s_package_valid = true;
    persist_nvs_unlocked();
}

void capture_session_mark_time_synced(void)
{
    s_time_synced = true;
}

bool capture_session_time_ok(void)
{
    /* 不要读 esp_sntp_get_sync_status()：COMPLETED 是一次性的，读完变 RESET。
     * 看板轮询 /api/capture 会把对时吃掉，界面一直停在「等待 NTP 对时」。 */
    return s_time_synced;
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
    int left = s_run_s - elapsed;
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
    if (s_room[0] == '\0' || s_event[0] == '\0') {
        load_nvs_unlocked();
    }
    if (s_event[0] == '\0') {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_ARG;
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
        s_run_s = s_duration_s;
        strncpy(s_run_event, s_event, sizeof(s_run_event) - 1);
        s_run_event[sizeof(s_run_event) - 1] = '\0';
        format_iso_local(s_started_at, s_run_start_iso, sizeof(s_run_start_iso));
        s_package_valid = false;
        s_active = true;
        s_last_sample_tick = 0;
        reset_logs_unlocked();
        xSemaphoreGive(s_lock);
    }
    ESP_LOGI(TAG, "start room=%s event=%s duration_s=%d t=%ld", s_room, s_run_event, s_run_s,
             (long)s_started_at);
    return ESP_OK;
}

void capture_session_stop(void)
{
    ensure_lock();
    if (s_lock != NULL && xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) == pdTRUE) {
        bool was_active = s_active;
        tear_down_unlocked();
        if (was_active) {
            commit_sample_unlocked();
            ESP_LOGI(TAG, "stop %s", s_last_sample_id);
        }
        xSemaphoreGive(s_lock);
    }
}

void capture_session_abort(void)
{
    ensure_lock();
    if (s_lock != NULL && xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) == pdTRUE) {
        if (s_active) {
            tear_down_unlocked();
            clear_logs_unlocked();
            s_package_valid = false;
            ESP_LOGW(TAG, "aborted");
        }
        xSemaphoreGive(s_lock);
    }
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

static void fmt_fixed2(char *dst, size_t dst_len, float v)
{
    int scaled = (int)(v * 100.0f + (v >= 0.0f ? 0.5f : -0.5f));
    int neg = scaled < 0;
    if (neg) {
        scaled = -scaled;
    }
    snprintf(dst, dst_len, "%s%d.%02d", neg ? "-" : "", scaled / 100, scaled % 100);
}

static void fmt_fixed1(char *dst, size_t dst_len, float v)
{
    int scaled = (int)(v * 10.0f + (v >= 0.0f ? 0.5f : -0.5f));
    int neg = scaled < 0;
    if (neg) {
        scaled = -scaled;
    }
    snprintf(dst, dst_len, "%s%d.%d", neg ? "-" : "", scaled / 10, scaled % 10);
}

static void format_wall_time(char *buf, size_t len)
{
    time_t now = time(NULL);
    struct tm tm_local;
    char wall[24];
    wall[0] = '\0';
    if (localtime_r(&now, &tm_local) != NULL) {
        strftime(wall, sizeof(wall), "%Y-%m-%d %H:%M:%S", &tm_local);
    }
    if (wall[0] == '\0') {
        snprintf(buf, len, "%ld", (long)now);
        return;
    }
    /* Excel 会把 "2026-09-04 14:53:00" 当成日期，列稍窄就显示 ######## */
    snprintf(buf, len, "=\"%s\"", wall);
}

static void capture_record_sample(void)
{
    static area_config_t cfg;
    static area_status_t areas[AREA_TAG_MAX];
    static c4004_target_info_t targets[TARGET_MAX];

    if (!s_active) {
        return;
    }

    TickType_t now_tick = xTaskGetTickCount();
    if (s_last_sample_tick != 0 &&
        (now_tick - s_last_sample_tick) < pdMS_TO_TICKS(900)) {
        return;
    }

    (void)c4004_sensor_refresh_targets();

    env_snapshot_t env = {0};
    (void)env_sensors_get(&env);

    float sound_db = 0.0f;
    bool sound_ok = false;
#if CONFIG_ENV_SOUND_ENABLE
    sound_ok = env_sound_get_db(&sound_db);
#endif

    memset(&cfg, 0, sizeof(cfg));
    (void)area_config_get(&cfg);

    uint8_t area_count = 0;
    (void)radar_area_get_status(areas, &area_count);

    uint8_t n_targets = 0;
    (void)c4004_sensor_get_targets(targets, TARGET_MAX, &n_targets);

    char room[CAPTURE_ROOM_LEN];
    capture_session_get_room(room, sizeof(room));
    char ts_field[32];
    format_wall_time(ts_field, sizeof(ts_field));

    char env_line[160];
    char t_c[16] = "";
    char h_c[16] = "";
    char co2_c[16] = "";
    char lux_c[16] = "";
    char snd_c[16] = "";
    if (env.env_ready) {
        fmt_fixed2(t_c, sizeof(t_c), env.temperature);
        fmt_fixed2(h_c, sizeof(h_c), env.humidity);
        snprintf(co2_c, sizeof(co2_c), "%d", env.co2);
    }
    if (env.lux_ready) {
        fmt_fixed1(lux_c, sizeof(lux_c), env.lux);
    }
    if (sound_ok) {
        fmt_fixed1(snd_c, sizeof(snd_c), sound_db);
    }
    snprintf(env_line, sizeof(env_line), "%s,%s,%s,%s,%s,%s\n",
             ts_field, t_c, h_c, co2_c, lux_c, snd_c);

    uint8_t n = cfg.tag_count < area_count ? cfg.tag_count : area_count;
    if (n > AREA_TAG_MAX) {
        n = AREA_TAG_MAX;
    }

    ensure_lock();
    if (s_lock == NULL || xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }
    if (!s_active) {
        xSemaphoreGive(s_lock);
        return;
    }
    s_last_sample_tick = now_tick;
    ensure_logs_unlocked();
    if (!buf_add(s_env_log, &s_env_len, ENV_LOG_CAP, env_line)) {
        ESP_LOGW(TAG, "env csv full");
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
            crossing = (int)c4004_sensor_take_boundary_crossing(i);
        } else {
            int prev_people = prev->valid ? prev->people : 0;
            if (prev_people == 0 && people > 0) {
                crossing = CROSSING_ENTER;
            } else if (prev_people > 0 && people == 0) {
                crossing = CROSSING_LEAVE;
            }
        }
        prev->people = people;
        prev->valid = true;

        const char *zname = cfg.tags[i].name[0] ? cfg.tags[i].name : areas[i].name;
        char zone_q[80];
        csv_quote(zone_q, sizeof(zone_q), zname ? zname : "");
        char room_q[80];
        csv_quote(room_q, sizeof(room_q), room);
        char line[256];
        snprintf(line, sizeof(line), "%s,%s,%s,%d,%d,%d,%d,%d\n",
                 ts_field, room_q, zone_q, buckets[0], buckets[1], buckets[2], buckets[3],
                 crossing);
        if (!buf_add(s_radar_log, &s_radar_len, RADAR_LOG_CAP, line)) {
            ESP_LOGW(TAG, "radar csv full");
            break;
        }
    }
    xSemaphoreGive(s_lock);
}

static void capture_tick_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (s_active) {
            capture_record_sample();
            expire_if_needed();
        }
    }
}

cJSON *capture_session_logs_json(void)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }
    const char *env = "";
    const char *radar = "";
    ensure_lock();
    if (s_lock != NULL && xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (s_env_log && s_env_log[0]) {
            env = s_env_log;
        }
        if (s_radar_log && s_radar_log[0]) {
            radar = s_radar_log;
        }
        cJSON_AddBoolToObject(root, "ok", true);
        cJSON_AddStringToObject(root, "env_csv", env);
        cJSON_AddStringToObject(root, "radar_csv", radar);
        if (s_package_valid) {
            cJSON_AddStringToObject(root, "sample_id", s_last_sample_id);
            cJSON_AddStringToObject(root, "start_time", s_last_start_iso);
            cJSON_AddStringToObject(root, "event", s_last_event);
            cJSON *label = cJSON_CreateObject();
            if (label != NULL) {
                cJSON_AddStringToObject(label, "sample_id", s_last_sample_id);
                cJSON_AddStringToObject(label, "start_time", s_last_start_iso);
                cJSON_AddStringToObject(label, "event", s_last_event);
                char *printed = cJSON_Print(label);
                cJSON_Delete(label);
                if (printed != NULL) {
                    cJSON_AddStringToObject(root, "label_json", printed);
                    cJSON_free(printed);
                }
            }
        }
        xSemaphoreGive(s_lock);
        return root;
    }
    cJSON_AddBoolToObject(root, "ok", false);
    return root;
}

cJSON *capture_session_status_json(void)
{
    expire_if_needed();
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }
    char room[CAPTURE_ROOM_LEN];
    char event[CAPTURE_EVENT_LEN];
    capture_session_get_room(room, sizeof(room));
    capture_session_get_event(event, sizeof(event));
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "time_ok", capture_session_time_ok());
    cJSON_AddBoolToObject(root, "recording", s_active);
    cJSON_AddStringToObject(root, "room", room);
    cJSON_AddStringToObject(root, "event", event);
    cJSON_AddNumberToObject(root, "sample_rate", AUDIO_CAPTURE_SAMPLE_RATE);
    cJSON_AddNumberToObject(root, "duration_s", capture_session_get_duration_s());
    cJSON_AddNumberToObject(root, "min_s", CAPTURE_SESSION_MIN_S);
    cJSON_AddNumberToObject(root, "max_s", CAPTURE_SESSION_MAX_S);
    cJSON_AddNumberToObject(root, "session_s", s_active ? s_run_s : 0);
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
            crossing = (int)c4004_sensor_take_boundary_crossing(i);
        } else {
            int prev_people = prev->valid ? prev->people : 0;
            if (prev_people == 0 && people > 0) {
                crossing = CROSSING_ENTER;
            } else if (prev_people > 0 && people == 0) {
                crossing = CROSSING_LEAVE;
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
