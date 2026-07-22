#include "yamnet_classifier.h"

#include <cstdio>
#include <cstring>
#include <cmath>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "yamnet_pcm_in.h"
#include "yamnet_mel.h"
#include "yamnet_events.h"

#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/system_setup.h"
#include "tensorflow/lite/schema/schema_generated.h"

static const char *TAG = "yamnet";

#define YAMNET_MOUNT "/yamnet"
#define YAMNET_MODEL_PATH "/yamnet/yamnet_cry_distill.tflite"
#define YAMNET_LABELS_PATH "/yamnet/yamnet_class_map.csv"
#define YAMNET_NUM_CLASSES 521
#define YAMNET_CLS_SPEECH  0
#define YAMNET_ARENA_SIZE (256 * 1024)
#define YAMNET_CATCHUP_KEEP_BYTES (19200U * sizeof(int16_t))

static uint8_t *s_model_buf;
static size_t s_model_len;
static uint8_t *s_arena;
static char (*s_labels)[48];
static int s_label_count;
static float s_probs[YAMNET_NUM_CLASSES];
static bool s_probs_valid;

static const tflite::Model *s_model;
static tflite::MicroInterpreter *s_interpreter;
static TfLiteTensor *s_input;
static TfLiteTensor *s_output;

static yamnet_mel_ctx_t *s_mel;
static TaskHandle_t s_infer_task;
static volatile bool s_running;
static yamnet_result_cb_t s_cb;
static void *s_cb_ctx;
static yamnet_event_app_cb_t s_event_cb;
static void *s_event_cb_ctx;
static SemaphoreHandle_t s_lock;
static yamnet_topk_t s_latest[YAMNET_TOP_K];
static int s_latest_count;
static int64_t s_last_infer_us;
static uint32_t s_infer_count;
static uint32_t s_skip_count;
static double s_rms_acc;
static size_t s_rms_n;

static void rms_accum_add(const int16_t *pcm, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        double s = (double)pcm[i];
        s_rms_acc += s * s;
    }
    s_rms_n += n;
}

static float rms_accum_take(void)
{
    float r = 0.0f;
    if (s_rms_n > 0) {
        r = (float)sqrt(s_rms_acc / (double)s_rms_n);
    }
    s_rms_acc = 0.0;
    s_rms_n = 0;
    return r;
}

static void catch_up_audio_if_needed(void)
{
    size_t pending = yamnet_pcm_in_bytes_pending();
    if (pending <= YAMNET_CATCHUP_KEEP_BYTES + 4096) {
        return;
    }
    size_t drop = pending - YAMNET_CATCHUP_KEEP_BYTES;
    yamnet_pcm_in_discard_bytes(drop);
    ESP_LOGW(TAG, "audio catch-up: dropped %u bytes (Invoke slower than realtime)", (unsigned)drop);
}

static void log_tensor_io_params(void)
{
    ESP_LOGI(TAG, "input quant scale=%.6f zp=%d", (double)s_input->params.scale, s_input->params.zero_point);
    ESP_LOGI(TAG, "output quant scale=%.6f zp=%d", (double)s_output->params.scale, s_output->params.zero_point);
    if (s_input->dims != NULL) {
        char dim_buf[48] = {0};
        int pos = 0;
        for (int i = 0; i < s_input->dims->size && pos < (int)sizeof(dim_buf) - 4; i++) {
            pos += snprintf(dim_buf + pos, sizeof(dim_buf) - (size_t)pos, "%s%d",
                            i ? "x" : "", s_input->dims->data[i]);
        }
        ESP_LOGI(TAG, "input shape [%s]", dim_buf);
    }
}

static esp_err_t mount_model_fs(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = YAMNET_MOUNT,
        .partition_label = "yamnet",
        .max_files = 4,
        .format_if_mount_failed = false,
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount yamnet partition failed: %s (run scripts/fetch_yamnet_model.ps1 then rebuild)",
                 esp_err_to_name(err));
    }
    return err;
}

static esp_err_t load_file(const char *path, uint8_t **out, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t *buf = (uint8_t *)heap_caps_malloc((size_t)sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        buf = (uint8_t *)malloc((size_t)sz);
    }
    if (buf == NULL) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (n != (size_t)sz) {
        free(buf);
        return ESP_FAIL;
    }
    *out = buf;
    *out_len = (size_t)sz;
    return ESP_OK;
}

static esp_err_t load_labels(void)
{
    FILE *f = fopen(YAMNET_LABELS_PATH, "r");
    if (f == NULL) {
        ESP_LOGW(TAG, "class map missing, will show class ids only");
        return ESP_ERR_NOT_FOUND;
    }

    s_labels = (char (*)[48])calloc(YAMNET_NUM_CLASSES, 48);
    if (s_labels == NULL) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    char line[128];
    /* skip header */
    (void)fgets(line, sizeof(line), f);
    int loaded = 0;
    while (fgets(line, sizeof(line), f) && loaded < YAMNET_NUM_CLASSES) {
        /* index,mid,display_name */
        char *p1 = strchr(line, ',');
        if (p1 == NULL) {
            continue;
        }
        char *p2 = strchr(p1 + 1, ',');
        if (p2 == NULL) {
            continue;
        }
        char *name = p2 + 1;
        /* strip quotes and newline */
        while (*name == '"' || *name == ' ') {
            name++;
        }
        size_t len = strlen(name);
        while (len > 0 && (name[len - 1] == '\n' || name[len - 1] == '\r' || name[len - 1] == '"')) {
            name[--len] = '\0';
        }
        strncpy(s_labels[loaded], name, 47);
        loaded++;
    }
    fclose(f);
    s_label_count = loaded;
    ESP_LOGI(TAG, "loaded %d class labels", s_label_count);
    return ESP_OK;
}

static void quantize_patch(const float *src, int8_t *dst, const TfLiteTensor *input)
{
    const float scale = input->params.scale;
    const int zp = input->params.zero_point;
    for (int i = 0; i < YAMNET_PATCH_ELEMS; i++) {
        int v = (int)lrintf(src[i] / scale) + zp;
        if (v < -128) {
            v = -128;
        }
        if (v > 127) {
            v = 127;
        }
        dst[i] = (int8_t)v;
    }
}

static void refresh_class_probs(void)
{
    const int n = s_output->dims->data[s_output->dims->size - 1];
    const int use_n = (n < YAMNET_NUM_CLASSES) ? n : YAMNET_NUM_CLASSES;
    s_probs_valid = false;
    if (use_n <= 0) {
        return;
    }

    /* Distill student: dequant logits then stable softmax (model card). */
    float max_l = -1e30f;
    for (int i = 0; i < use_n; i++) {
        float logit;
        if (s_output->type == kTfLiteInt8) {
            logit = (s_output->data.int8[i] - s_output->params.zero_point) * s_output->params.scale;
        } else if (s_output->type == kTfLiteUInt8) {
            logit = (s_output->data.uint8[i] - s_output->params.zero_point) * s_output->params.scale;
        } else {
            logit = s_output->data.f[i];
        }
        s_probs[i] = logit;
        if (logit > max_l) {
            max_l = logit;
        }
    }
    float sum = 0.0f;
    for (int i = 0; i < use_n; i++) {
        s_probs[i] = expf(s_probs[i] - max_l);
        sum += s_probs[i];
    }
    if (sum > 0.0f) {
        for (int i = 0; i < use_n; i++) {
            s_probs[i] /= sum;
        }
    }
    for (int i = use_n; i < YAMNET_NUM_CLASSES; i++) {
        s_probs[i] = 0.0f;
    }
    s_probs_valid = true;
}

static float class_score(int class_id)
{
    if (!s_probs_valid || class_id < 0 || class_id >= YAMNET_NUM_CLASSES) {
        return 0.0f;
    }
    return s_probs[class_id];
}

static void on_event_alert(const yamnet_event_desc_t *ev, float score, int votes, void *user_ctx)
{
    (void)user_ctx;
    if (s_event_cb && ev != NULL) {
        s_event_cb(ev->id, score, votes, s_event_cb_ctx);
    }
}

static void collect_topk(yamnet_topk_t *out, int *out_n)
{
    struct Item {
        int id;
        float score;
    };
    Item best[YAMNET_TOP_K];
    for (int i = 0; i < YAMNET_TOP_K; i++) {
        best[i] = {-1, -1.0f};
    }

    const int n = s_output->dims->data[s_output->dims->size - 1];
    for (int i = 0; i < n; i++) {
        float score = class_score(i);

        for (int k = 0; k < YAMNET_TOP_K; k++) {
            if (score > best[k].score) {
                for (int j = YAMNET_TOP_K - 1; j > k; j--) {
                    best[j] = best[j - 1];
                }
                best[k] = {i, score};
                break;
            }
        }
    }

    int count = 0;
    for (int i = 0; i < YAMNET_TOP_K; i++) {
        if (best[i].id < 0) {
            break;
        }
        out[count].class_id = best[i].id;
        out[count].score = best[i].score;
        if (s_labels != NULL && best[i].id < s_label_count) {
            strncpy(out[count].name, s_labels[best[i].id], sizeof(out[count].name) - 1);
        } else {
            snprintf(out[count].name, sizeof(out[count].name), "class_%d", best[i].id);
        }
        count++;
    }
    *out_n = count;
}

static int output_class_count(void)
{
    if (s_output == NULL || s_output->dims == NULL) {
        return 0;
    }
    int n = 1;
    for (int i = 0; i < s_output->dims->size; i++) {
        n *= s_output->dims->data[i];
    }
    return n;
}

static void log_output_stats(const char *tag)
{
    const int n = output_class_count();
    if (n <= 0) {
        return;
    }
    int raw_min = 127;
    int raw_max = -128;
    int count64 = 0;
    float score_max = -1.0f;
    int score_max_id = -1;
    for (int i = 0; i < n; i++) {
        int8_t raw = s_output->data.int8[i];
        if (raw < raw_min) {
            raw_min = raw;
        }
        if (raw > raw_max) {
            raw_max = raw;
        }
        if (raw == 64) {
            count64++;
        }
        float score = class_score(i);
        if (score > score_max) {
            score_max = score;
            score_max_id = i;
        }
    }
    ESP_LOGI(TAG,
             "%s out_raw[%d..%d] raw=64:%d/%d max=%.3f@id%d cry=%.3f glass=%.3f",
             tag, raw_min, raw_max, count64, n, score_max, score_max_id,
             yamnet_events_score("cry", s_probs, YAMNET_NUM_CLASSES),
             yamnet_events_score("glass", s_probs, YAMNET_NUM_CLASSES));
}

static void run_sanity_infer(const char *tag, const float *patch)
{
    if (s_input->type == kTfLiteInt8) {
        quantize_patch(patch, s_input->data.int8, s_input);
    } else if (s_input->type == kTfLiteFloat32) {
        memcpy(s_input->data.f, patch, sizeof(float) * YAMNET_PATCH_ELEMS);
    } else {
        ESP_LOGE(TAG, "sanity: bad input type %d", (int)s_input->type);
        return;
    }
    int64_t t0 = esp_timer_get_time();
    if (s_interpreter->Invoke() != kTfLiteOk) {
        ESP_LOGE(TAG, "sanity Invoke failed (%s)", tag);
        return;
    }
    refresh_class_probs();
    /* Let IDLE run between long Invokes */
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_LOGI(TAG, "sanity %s took %lld ms", tag, (long long)((esp_timer_get_time() - t0) / 1000));
    log_output_stats(tag);
}

static void run_startup_sanity_tests(void)
{
    float *patch = (float *)malloc(sizeof(float) * YAMNET_PATCH_ELEMS);
    if (patch == NULL) {
        return;
    }
    for (int i = 0; i < YAMNET_PATCH_ELEMS; i++) {
        patch[i] = -5.0f;
    }
    run_sanity_infer("sanity_flat_-5", patch);

    for (int i = 0; i < YAMNET_PATCH_ELEMS; i++) {
        patch[i] = -5.0f + 3.0f * sinf(0.07f * (float)i);
    }
    run_sanity_infer("sanity_sine", patch);
    free(patch);
}

static void infer_task(void *arg)
{
    (void)arg;
    int16_t pcm[512];
    float *patch = (float *)heap_caps_malloc(sizeof(float) * YAMNET_PATCH_ELEMS,
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (patch == NULL) {
        patch = (float *)malloc(sizeof(float) * YAMNET_PATCH_ELEMS);
    }
    if (patch == NULL) {
        ESP_LOGE(TAG, "patch buffer alloc failed");
        s_infer_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "inference task running");
    /* Do NOT subscribe this task to TWDT: Invoke lasts ~1.9s and wait/silence
     * paths may idle longer than the timeout. Yield often so IDLE0 can run. */

    run_startup_sanity_tests();

    while (s_running) {
        catch_up_audio_if_needed();

        size_t n = yamnet_pcm_in_read(pcm, sizeof(pcm) / sizeof(pcm[0]));
        if (n == 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        rms_accum_add(pcm, n);
        (void)yamnet_mel_push_pcm(s_mel, pcm, n);

        if (!yamnet_mel_patch_ready(s_mel)) {
            /* Yield while filling the first / next mel patch */
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        int64_t now_us = esp_timer_get_time();
        int64_t min_gap_us = (int64_t)CONFIG_YAMNET_INFER_MIN_INTERVAL_MS * 1000;
        if (s_last_infer_us > 0 && (now_us - s_last_infer_us) < min_gap_us) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (yamnet_mel_export_patch(s_mel, patch, (uint8_t)CONFIG_YAMNET_INFER_HOP_FRAMES) != ESP_OK) {
            continue;
        }

        float window_rms = rms_accum_take();
#if CONFIG_YAMNET_INFER_RMS_MIN > 0
        if (window_rms < (float)CONFIG_YAMNET_INFER_RMS_MIN) {
            s_skip_count++;
            s_last_infer_us = esp_timer_get_time();
            if ((s_skip_count % 10) == 1) {
                ESP_LOGI(TAG, "skip Infer (silence) pcm_rms=%.0f < %d",
                         window_rms, CONFIG_YAMNET_INFER_RMS_MIN);
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
#endif

        if (s_input->type == kTfLiteInt8) {
            quantize_patch(patch, s_input->data.int8, s_input);
        } else if (s_input->type == kTfLiteFloat32) {
            memcpy(s_input->data.f, patch, sizeof(float) * YAMNET_PATCH_ELEMS);
        } else {
            ESP_LOGE(TAG, "unsupported input type %d", (int)s_input->type);
            break;
        }

        int64_t t0 = esp_timer_get_time();
        if (s_interpreter->Invoke() != kTfLiteOk) {
            ESP_LOGE(TAG, "Invoke failed");
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        refresh_class_probs();
        int64_t dt_ms = (esp_timer_get_time() - t0) / 1000;
        s_last_infer_us = esp_timer_get_time();
        s_infer_count++;

        yamnet_topk_t top[YAMNET_TOP_K];
        int top_n = 0;
        collect_topk(top, &top_n);

        const float speech = class_score(YAMNET_CLS_SPEECH);
        yamnet_events_on_probs(s_probs, YAMNET_NUM_CLASSES, speech, on_event_alert, NULL);

        if (s_lock) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            memcpy(s_latest, top, sizeof(top));
            s_latest_count = top_n;
            xSemaphoreGive(s_lock);
        }

        const float cry = yamnet_events_score("cry", s_probs, YAMNET_NUM_CLASSES);
        const float glass = yamnet_events_score("glass", s_probs, YAMNET_NUM_CLASSES);
        const bool interesting = (cry >= 0.10f) || (glass >= 0.08f);

        const bool log_detail =
#if CONFIG_YAMNET_LOG_VERBOSE
            true;
#else
            interesting || (s_infer_count <= 3) || ((s_infer_count % 30) == 0);
#endif
        if (log_detail) {
            if (s_infer_count <= 5 || (s_infer_count % 20) == 0) {
                log_output_stats("live");
            }
            ESP_LOGI(TAG,
                     "infer %lld ms | pcm_rms=%.0f cry=%.3f glass=%.3f Speech=%.3f | "
                     "#1 %s (%.3f) | #2 %s (%.3f) | #3 %s (%.3f)",
                     (long long)dt_ms, window_rms, cry, glass, speech,
                     top_n > 0 ? top[0].name : "-", top_n > 0 ? top[0].score : 0.0f,
                     top_n > 1 ? top[1].name : "-", top_n > 1 ? top[1].score : 0.0f,
                     top_n > 2 ? top[2].name : "-", top_n > 2 ? top[2].score : 0.0f);
        }

        if (s_cb) {
            s_cb(top, top_n, s_cb_ctx);
        }

        catch_up_audio_if_needed();
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    free(patch);
    s_infer_task = NULL;
    vTaskDelete(NULL);
}

extern "C" esp_err_t yamnet_classifier_start(yamnet_result_cb_t cb, void *user_ctx)
{
    if (s_running) {
        return ESP_OK;
    }

    tflite::InitializeTarget();

    s_cb = cb;
    s_cb_ctx = user_ctx;
    s_lock = xSemaphoreCreateMutex();
    yamnet_events_init();

    esp_err_t err = mount_model_fs();
    if (err != ESP_OK) {
        return err;
    }

    err = load_file(YAMNET_MODEL_PATH, &s_model_buf, &s_model_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "load model failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "model loaded %u bytes (%s)", (unsigned)s_model_len, YAMNET_MODEL_PATH);
#if CONFIG_YAMNET_DISABLE_ESP_NN
    ESP_LOGI(TAG, "TFLite kernels: reference (CONFIG_YAMNET_DISABLE_ESP_NN)");
#elif CONFIG_NN_ANSI_C
    ESP_LOGI(TAG, "TFLite kernels: ESP-NN ANSI");
#else
    ESP_LOGI(TAG, "TFLite kernels: ESP-NN optimized");
#endif
    ESP_LOGI(TAG, "output post: dequant + softmax; cry = p[19]+p[20] (thr~0.05)");
    (void)load_labels();

    s_arena = (uint8_t *)heap_caps_malloc(YAMNET_ARENA_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_arena == NULL) {
        s_arena = (uint8_t *)heap_caps_malloc(YAMNET_ARENA_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (s_arena == NULL) {
        ESP_LOGE(TAG, "tensor arena %d bytes alloc failed (enable SPIRAM)", YAMNET_ARENA_SIZE);
        return ESP_ERR_NO_MEM;
    }

    s_model = tflite::GetModel(s_model_buf);
    if (s_model->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE(TAG, "model schema mismatch");
        return ESP_ERR_INVALID_VERSION;
    }

    static tflite::MicroMutableOpResolver<16> resolver;
    resolver.AddConv2D();
    resolver.AddDepthwiseConv2D();
    resolver.AddFullyConnected();
    resolver.AddAveragePool2D();
    resolver.AddMaxPool2D();
    resolver.AddMean();
    resolver.AddReshape();
    resolver.AddQuantize();
    resolver.AddDequantize();
    resolver.AddSoftmax();
    resolver.AddLogistic();
    resolver.AddPad();
    resolver.AddAdd();
    resolver.AddMul();
    resolver.AddRelu();
    resolver.AddRelu6();

    static tflite::MicroInterpreter interpreter(s_model, resolver, s_arena, YAMNET_ARENA_SIZE);
    s_interpreter = &interpreter;

    if (s_interpreter->AllocateTensors() != kTfLiteOk) {
        ESP_LOGE(TAG, "AllocateTensors failed (increase arena or enable PSRAM)");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "arena used ~%u bytes", (unsigned)s_interpreter->arena_used_bytes());

    s_input = s_interpreter->input(0);
    s_output = s_interpreter->output(0);
    ESP_LOGI(TAG, "input type=%d dims=%d output type=%d outputs=%u",
             (int)s_input->type, s_input->dims->size, (int)s_output->type,
             (unsigned)s_interpreter->outputs_size());
    log_tensor_io_params();

    s_mel = yamnet_mel_create();
    if (s_mel == NULL) {
        return ESP_ERR_NO_MEM;
    }

    err = yamnet_pcm_in_start();
    if (err != ESP_OK) {
        return err;
    }

    s_running = true;
    BaseType_t task_ok;
#if CONFIG_YAMNET_INFER_TASK_CORE >= 0
    task_ok = xTaskCreatePinnedToCore(infer_task, "yamnet_inf", 12288, NULL, 2, &s_infer_task,
                                      CONFIG_YAMNET_INFER_TASK_CORE);
#else
    task_ok = xTaskCreate(infer_task, "yamnet_inf", 12288, NULL, 2, &s_infer_task);
#endif
    if (task_ok != pdPASS) {
        s_running = false;
        yamnet_pcm_in_stop();
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "YAMNet classifier started");
    return ESP_OK;
}

extern "C" void yamnet_classifier_stop(void)
{
    if (!s_running) {
        return;
    }
    s_running = false;
    while (s_infer_task != NULL) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    yamnet_pcm_in_stop();
    yamnet_mel_destroy(s_mel);
    s_mel = NULL;
}

extern "C" esp_err_t yamnet_classifier_get_latest(yamnet_topk_t *out, int *out_count)
{
    if (out == NULL || out_count == NULL || s_lock == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = s_latest_count;
    memcpy(out, s_latest, sizeof(yamnet_topk_t) * (size_t)n);
    xSemaphoreGive(s_lock);
    *out_count = n;
    return ESP_OK;
}

extern "C" void yamnet_classifier_set_event_cb(yamnet_event_app_cb_t cb, void *user_ctx)
{
    s_event_cb = cb;
    s_event_cb_ctx = user_ctx;
}
