/**
 * @file env_sound.c
 * @brief ES7210 环境声级：PCM RMS → 近似 dB SPL
 */
#include "env_sound.h"

#include "audio_capture.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

static const char *TAG = "env_sound";

#ifndef CONFIG_ENV_SOUND_REF_DB
#define CONFIG_ENV_SOUND_REF_DB 94
#endif

#ifndef CONFIG_ENV_SOUND_EMA_ALPHA_MILLI
#define CONFIG_ENV_SOUND_EMA_ALPHA_MILLI 200
#endif

static SemaphoreHandle_t s_lock;
static int s_sink_id = -1;
static bool s_started;
static bool s_have_level;
static float s_ema_rms;
static float s_level_db;

static void ensure_lock(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
}

static float rms_of(const int16_t *pcm, size_t n)
{
    if (pcm == NULL || n == 0) {
        return 0.0f;
    }
    double acc = 0.0;
    for (size_t i = 0; i < n; i++) {
        double s = (double)pcm[i];
        acc += s * s;
    }
    return (float)sqrt(acc / (double)n);
}

static float rms_to_db(float rms)
{
#if CONFIG_AUDIO_CAPTURE_DIGITAL_GAIN > 1
    rms /= (float)CONFIG_AUDIO_CAPTURE_DIGITAL_GAIN;
#endif
    const float safe = fmaxf(rms, 1.0f);
    float db = 20.0f * log10f(safe / 32768.0f) + (float)CONFIG_ENV_SOUND_REF_DB;
    if (db < 0.0f) {
        db = 0.0f;
    } else if (db > 140.0f) {
        db = 140.0f;
    }
    return db;
}

static void on_pcm(const int16_t *pcm, size_t n_samples, void *ctx)
{
    (void)ctx;
    float chunk_rms = rms_of(pcm, n_samples);
    const float alpha = (float)CONFIG_ENV_SOUND_EMA_ALPHA_MILLI / 1000.0f;

    ensure_lock();
    if (s_lock != NULL && xSemaphoreTake(s_lock, pdMS_TO_TICKS(5)) == pdTRUE) {
        if (!s_have_level) {
            s_ema_rms = chunk_rms;
        } else {
            s_ema_rms = (1.0f - alpha) * s_ema_rms + alpha * chunk_rms;
        }
        s_level_db = rms_to_db(s_ema_rms);
        s_have_level = true;
        xSemaphoreGive(s_lock);
    }
}

esp_err_t env_sound_start(void)
{
    ensure_lock();
    if (s_started) {
        return ESP_OK;
    }

    esp_err_t err = audio_capture_acquire();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "capture acquire failed: %s", esp_err_to_name(err));
        return err;
    }

    err = audio_capture_add_sink(on_pcm, NULL, &s_sink_id);
    if (err != ESP_OK) {
        audio_capture_release();
        s_sink_id = -1;
        ESP_LOGE(TAG, "add sink failed: %s", esp_err_to_name(err));
        return err;
    }

    s_have_level = false;
    s_ema_rms = 0.0f;
    s_level_db = 0.0f;
    s_started = true;
    ESP_LOGI(TAG, "started (ref_db=%d, fs=%u)", CONFIG_ENV_SOUND_REF_DB,
             (unsigned)audio_capture_sample_rate());
    return ESP_OK;
}

void env_sound_stop(void)
{
    if (!s_started) {
        return;
    }
    if (s_sink_id >= 0) {
        audio_capture_remove_sink(s_sink_id);
        s_sink_id = -1;
    }
    audio_capture_release();
    s_started = false;
    s_have_level = false;
}

bool env_sound_ready(void)
{
    return s_started && s_have_level;
}

bool env_sound_get_db(float *out_db)
{
    if (out_db == NULL || !s_started || !s_have_level) {
        return false;
    }
    ensure_lock();
    if (s_lock != NULL && xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        *out_db = s_level_db;
        xSemaphoreGive(s_lock);
        return true;
    }
    *out_db = s_level_db;
    return true;
}
