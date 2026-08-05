/**
 * @file esp_sr_wake.c
 * @brief 单麦 AFE + WakeNet；与 YAMNet / stream 解耦，仅订阅 audio_capture
 */
#include "esp_sr_wake.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if CONFIG_ESP_SR_WAKE_ENABLE

#include "audio_capture.h"
#include "esp_afe_sr_models.h"
#include "esp_heap_caps.h"
#include "model_path.h"

#if CONFIG_ESP_SR_WAKE_START_STREAM && CONFIG_AUDIO_WS_ENABLE
#include "audio_ws_client.h"
#endif
#if CONFIG_ESP_SR_WAKE_START_STREAM && CONFIG_AUDIO_STREAM_ENABLE
#include "audio_stream.h"
#endif

static const char *TAG = "esp_sr_wake";

#if CONFIG_ESP_SR_WAKE_START_STREAM && CONFIG_AUDIO_WS_ENABLE
static int64_t s_vad_silence_us;
static bool s_vad_heard_speech;
#endif

/* 约 1s 的 mono PCM，供 AFE feed 追赶 */
#define WAKE_PCM_RB_BYTES (AUDIO_CAPTURE_SAMPLE_RATE * sizeof(int16_t))

static srmodel_list_t *s_models;
static afe_config_t *s_afe_cfg;
static const esp_afe_sr_iface_t *s_afe_handle;
static esp_afe_sr_data_t *s_afe_data;

static RingbufHandle_t s_pcm_rb;
static int s_sink_id = -1;
static TaskHandle_t s_feed_task;
static TaskHandle_t s_fetch_task;
static volatile bool s_running;
static esp_sr_wake_cb_t s_wake_cb;
static void *s_wake_cb_ctx;
static bool s_started;

static void make_room(size_t need_bytes)
{
    if (s_pcm_rb == NULL) {
        return;
    }
    for (;;) {
        UBaseType_t waiting = 0;
        vRingbufferGetInfo(s_pcm_rb, NULL, NULL, NULL, NULL, &waiting);
        if ((size_t)waiting + need_bytes <= WAKE_PCM_RB_BYTES) {
            break;
        }
        size_t item_size = 0;
        void *item = xRingbufferReceiveUpTo(s_pcm_rb, &item_size, 0, need_bytes);
        if (item == NULL) {
            break;
        }
        vRingbufferReturnItem(s_pcm_rb, item);
    }
}

/** 采集 sink：PCM 入私有 ring，不在回调里跑 AFE。 */
static void on_capture_pcm(const int16_t *pcm, size_t n_samples, void *ctx)
{
    (void)ctx;
    if (!s_running || s_pcm_rb == NULL || pcm == NULL || n_samples == 0) {
        return;
    }
    const size_t nbytes = n_samples * sizeof(int16_t);
    make_room(nbytes);
    (void)xRingbufferSend(s_pcm_rb, pcm, nbytes, 0);
}

/** 从 ring 凑满 feed_chunksize 后送入 AFE。 */
static void feed_task(void *arg)
{
    (void)arg;
    const int chunk = s_afe_handle->get_feed_chunksize(s_afe_data);
    const int nch = s_afe_handle->get_feed_channel_num(s_afe_data);
    const size_t need = (size_t)chunk * (size_t)nch * sizeof(int16_t);

    int16_t *feed_buf = (int16_t *)heap_caps_malloc(need, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (feed_buf == NULL) {
        feed_buf = (int16_t *)malloc(need);
    }
    if (feed_buf == NULL) {
        ESP_LOGE(TAG, "feed buffer alloc failed");
        s_feed_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "feed task: chunk=%d nch=%d", chunk, nch);

    size_t filled = 0;
    while (s_running) {
        size_t want = need - filled;
        size_t item_size = 0;
        void *item = xRingbufferReceiveUpTo(s_pcm_rb, &item_size, pdMS_TO_TICKS(50), want);
        if (item == NULL) {
            continue;
        }
        memcpy((uint8_t *)feed_buf + filled, item, item_size);
        filled += item_size;
        vRingbufferReturnItem(s_pcm_rb, item);

        if (filled >= need) {
            s_afe_handle->feed(s_afe_data, feed_buf);
            filled = 0;
        }
    }

    free(feed_buf);
    s_feed_task = NULL;
    vTaskDelete(NULL);
}

/** 拉取 AFE 结果；检测到唤醒词则可选启动透传；上行中用 VAD 判停说。 */
static void fetch_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "fetch task started");

    while (s_running) {
        afe_fetch_result_t *res = s_afe_handle->fetch_with_delay(s_afe_data, pdMS_TO_TICKS(200));
        if (res == NULL || res->ret_value == ESP_FAIL) {
            continue;
        }

        if (res->wakeup_state == WAKENET_DETECTED) {
            ESP_LOGI(TAG, "WAKEWORD DETECTED (word_index=%d model_index=%d)",
                     res->wake_word_index, res->wakenet_model_index);
            if (s_wake_cb) {
                s_wake_cb(s_wake_cb_ctx);
            }
#if CONFIG_ESP_SR_WAKE_START_STREAM
#if CONFIG_AUDIO_WS_ENABLE
            /* 上行/等 TTS/播放中禁止；续听窗口不算 busy，可直接开上行 */
            if (!audio_ws_client_is_busy()) {
                s_vad_silence_us = 0;
                s_vad_heard_speech = false;
                esp_err_t err = audio_ws_client_is_followup_armed()
                                    ? audio_ws_client_followup_begin()
                                    : audio_ws_client_session_start();
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "audio_ws_client_session_start: %s", esp_err_to_name(err));
                }
            } else {
                ESP_LOGI(TAG, "wake ignored — WS session busy");
            }
#elif CONFIG_AUDIO_STREAM_ENABLE
            esp_err_t err = audio_stream_start();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "audio_stream_start: %s", esp_err_to_name(err));
            }
#endif
#endif
        }

#if CONFIG_ESP_SR_WAKE_START_STREAM && CONFIG_AUDIO_WS_ENABLE
        if (audio_ws_client_is_followup_armed() && !audio_ws_client_is_busy() &&
            !audio_ws_client_is_uplink_active()) {
            if (res->vad_state == VAD_SPEECH) {
                ESP_LOGI(TAG, "followup speech — begin uplink");
                s_vad_silence_us = 0;
                s_vad_heard_speech = true;
                esp_err_t err = audio_ws_client_followup_begin();
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "followup_begin: %s", esp_err_to_name(err));
                    s_vad_heard_speech = false;
                } else {
                    audio_ws_client_notify_speech_heard();
                }
            }
        }

        if (audio_ws_client_is_uplink_active() && !audio_ws_client_is_playing()) {
            if (res->vad_state == VAD_SPEECH) {
                if (!s_vad_heard_speech) {
                    audio_ws_client_notify_speech_heard();
                }
                s_vad_heard_speech = true;
                s_vad_silence_us = 0;
            } else if (res->vad_state == VAD_SILENCE) {
                /* 必须先出现过语音，再累计静音 — 避免唤醒后未说话就 EOS */
                if (!s_vad_heard_speech) {
                    s_vad_silence_us = 0;
                } else {
                    int samples = res->data_size > 0 ? (res->data_size / (int)sizeof(int16_t)) : 0;
                    if (samples > 0) {
                        s_vad_silence_us +=
                            (int64_t)samples * 1000000LL / (int64_t)AUDIO_CAPTURE_SAMPLE_RATE;
                    } else {
                        s_vad_silence_us += 200000;
                    }
                    if (s_vad_silence_us >= (int64_t)CONFIG_AUDIO_WS_EOS_MS * 1000LL) {
                        ESP_LOGI(TAG, "VAD EOS (silence >= %d ms)", CONFIG_AUDIO_WS_EOS_MS);
                        audio_ws_client_notify_eos();
                        s_vad_silence_us = 0;
                        s_vad_heard_speech = false;
                    }
                }
            }
        } else if (!audio_ws_client_is_uplink_active()) {
            s_vad_silence_us = 0;
        }
#endif
    }

    s_fetch_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t esp_sr_wake_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    s_models = esp_srmodel_init("model");
    if (s_models == NULL) {
        ESP_LOGE(TAG, "esp_srmodel_init(model) failed — check model partition & menuconfig WakeNet");
        return ESP_FAIL;
    }

    /* 单麦、无参考：关闭 AEC；唤醒用 SR 路径 */
    s_afe_cfg = afe_config_init("M", s_models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    if (s_afe_cfg == NULL) {
        ESP_LOGE(TAG, "afe_config_init failed");
        esp_srmodel_deinit(s_models);
        s_models = NULL;
        return ESP_FAIL;
    }
    s_afe_cfg->aec_init = false;
    s_afe_cfg->se_init = false;
    s_afe_cfg->vad_init = true;
    s_afe_cfg->wakenet_init = true;

    if (s_afe_cfg->wakenet_model_name) {
        ESP_LOGI(TAG, "WakeNet model: %s", s_afe_cfg->wakenet_model_name);
    } else {
        ESP_LOGW(TAG, "no WakeNet model name — enable one under ESP Speech Recognition");
    }

    s_afe_handle = esp_afe_handle_from_config(s_afe_cfg);
    s_afe_data = s_afe_handle->create_from_config(s_afe_cfg);
    if (s_afe_data == NULL) {
        ESP_LOGE(TAG, "create_from_config failed");
        afe_config_free(s_afe_cfg);
        s_afe_cfg = NULL;
        esp_srmodel_deinit(s_models);
        s_models = NULL;
        return ESP_FAIL;
    }

    s_pcm_rb = xRingbufferCreateWithCaps(WAKE_PCM_RB_BYTES, RINGBUF_TYPE_BYTEBUF,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_pcm_rb == NULL) {
        s_pcm_rb = xRingbufferCreateWithCaps(WAKE_PCM_RB_BYTES, RINGBUF_TYPE_BYTEBUF,
                                             MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (s_pcm_rb == NULL) {
        s_afe_handle->destroy(s_afe_data);
        s_afe_data = NULL;
        afe_config_free(s_afe_cfg);
        s_afe_cfg = NULL;
        esp_srmodel_deinit(s_models);
        s_models = NULL;
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = audio_capture_acquire();
    if (err != ESP_OK) {
        vRingbufferDeleteWithCaps(s_pcm_rb);
        s_pcm_rb = NULL;
        s_afe_handle->destroy(s_afe_data);
        s_afe_data = NULL;
        afe_config_free(s_afe_cfg);
        s_afe_cfg = NULL;
        esp_srmodel_deinit(s_models);
        s_models = NULL;
        return err;
    }

    err = audio_capture_add_sink(on_capture_pcm, NULL, &s_sink_id);
    if (err != ESP_OK) {
        audio_capture_release();
        vRingbufferDeleteWithCaps(s_pcm_rb);
        s_pcm_rb = NULL;
        s_afe_handle->destroy(s_afe_data);
        s_afe_data = NULL;
        afe_config_free(s_afe_cfg);
        s_afe_cfg = NULL;
        esp_srmodel_deinit(s_models);
        s_models = NULL;
        return err;
    }

    s_running = true;
    s_started = true;

    if (xTaskCreate(feed_task, "sr_feed", 4096, NULL, 5, &s_feed_task) != pdPASS) {
        esp_sr_wake_stop();
        return ESP_ERR_NO_MEM;
    }

#if CONFIG_ESP_SR_WAKE_FETCH_CORE >= 0
    if (xTaskCreatePinnedToCore(fetch_task, "sr_fetch", 8192, NULL, 5, &s_fetch_task,
                                CONFIG_ESP_SR_WAKE_FETCH_CORE) != pdPASS) {
        esp_sr_wake_stop();
        return ESP_ERR_NO_MEM;
    }
#else
    if (xTaskCreate(fetch_task, "sr_fetch", 8192, NULL, 5, &s_fetch_task) != pdPASS) {
        esp_sr_wake_stop();
        return ESP_ERR_NO_MEM;
    }
#endif

    ESP_LOGI(TAG, "esp-sr wake started (sink=%d)", s_sink_id);
    return ESP_OK;
}

void esp_sr_wake_stop(void)
{
    if (!s_started && !s_running) {
        return;
    }
    s_running = false;
    s_started = false;

    while (s_feed_task != NULL || s_fetch_task != NULL) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    if (s_sink_id >= 0) {
        audio_capture_remove_sink(s_sink_id);
        s_sink_id = -1;
        audio_capture_release();
    }

    if (s_pcm_rb != NULL) {
        vRingbufferDeleteWithCaps(s_pcm_rb);
        s_pcm_rb = NULL;
    }

    if (s_afe_handle != NULL && s_afe_data != NULL) {
        s_afe_handle->destroy(s_afe_data);
        s_afe_data = NULL;
    }
    if (s_afe_cfg != NULL) {
        afe_config_free(s_afe_cfg);
        s_afe_cfg = NULL;
    }
    if (s_models != NULL) {
        esp_srmodel_deinit(s_models);
        s_models = NULL;
    }
    s_afe_handle = NULL;
    ESP_LOGI(TAG, "esp-sr wake stopped");
}

bool esp_sr_wake_is_running(void)
{
    return s_started;
}

void esp_sr_wake_set_callback(esp_sr_wake_cb_t cb, void *user_ctx)
{
    s_wake_cb = cb;
    s_wake_cb_ctx = user_ctx;
}

#else /* !CONFIG_ESP_SR_WAKE_ENABLE */

esp_err_t esp_sr_wake_start(void)
{
    ESP_LOGW("esp_sr_wake", "ESP_SR_WAKE_ENABLE is off");
    return ESP_ERR_NOT_SUPPORTED;
}

void esp_sr_wake_stop(void) {}

bool esp_sr_wake_is_running(void)
{
    return false;
}

void esp_sr_wake_set_callback(esp_sr_wake_cb_t cb, void *user_ctx)
{
    (void)cb;
    (void)user_ctx;
}

#endif /* CONFIG_ESP_SR_WAKE_ENABLE */
