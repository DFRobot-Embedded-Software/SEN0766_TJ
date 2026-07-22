#include "yamnet_mel.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_dsp.h"
#include "sdkconfig.h"

static const char *TAG = "yamnet_mel";

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Match chayuto/ws-ESP32-S3-CAM cry-detect-01 mel_features.c (YAMNet PTQ calibration). */
#define YAMNET_LOG_OFFSET 0.001f
#define YAMNET_FMIN_HZ    125.0f
#define YAMNET_FMAX_HZ    7500.0f

struct yamnet_mel_ctx {
    float *window;
    float *fft_buf;
    float *magnitude;
    float *mel_filters;
    float *frame_ring;
    int16_t *pcm_ring;
    size_t pcm_len;
    size_t pcm_cap;
    uint8_t frame_count;
    uint8_t frame_write;
    uint8_t frames_since_export;
    bool fft_ok;
};

static float hz_to_mel(float hz)
{
    return 1127.0f * logf(1.0f + hz / 700.0f);
}

static float mel_to_hz(float mel)
{
    return 700.0f * (expf(mel / 1127.0f) - 1.0f);
}

static void build_mel_filters(float *filters)
{
    const int n_fft = YAMNET_FFT_SIZE;
    const int n_freqs = n_fft / 2 + 1;
    const int n_mels = YAMNET_MEL_BANDS;
    const float sr = (float)YAMNET_SAMPLE_RATE;

    float mel_min = hz_to_mel(YAMNET_FMIN_HZ);
    float mel_max = hz_to_mel(YAMNET_FMAX_HZ);
    float mel_pts[YAMNET_MEL_BANDS + 2];
    float hz_pts[YAMNET_MEL_BANDS + 2];
    float bin_pts[YAMNET_MEL_BANDS + 2];

    for (int i = 0; i < n_mels + 2; i++) {
        mel_pts[i] = mel_min + (mel_max - mel_min) * (float)i / (float)(n_mels + 1);
        hz_pts[i] = mel_to_hz(mel_pts[i]);
        bin_pts[i] = hz_pts[i] * (float)n_fft / sr;
    }

    memset(filters, 0, sizeof(float) * (size_t)n_mels * (size_t)n_freqs);
    for (int m = 1; m <= n_mels; m++) {
        const float left = bin_pts[m - 1];
        const float center = bin_pts[m];
        const float right = bin_pts[m + 1];
        float *row = &filters[(m - 1) * n_freqs];

        for (int k = 0; k < n_freqs; k++) {
            float fk = (float)k;
            float w = 0.0f;
            if (fk >= left && fk <= right) {
                if (fk < center) {
                    w = (center != left) ? (fk - left) / (center - left) : 0.0f;
                } else {
                    w = (right != center) ? (right - fk) / (right - center) : 0.0f;
                }
            }
            row[k] = w;
        }
    }
}

yamnet_mel_ctx_t *yamnet_mel_create(void)
{
    yamnet_mel_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return NULL;
    }

    const size_t n_freqs = YAMNET_FFT_SIZE / 2 + 1;
    ctx->window = calloc(YAMNET_WINDOW_SAMPLES, sizeof(float));
    ctx->fft_buf = heap_caps_calloc(1, sizeof(float) * YAMNET_FFT_SIZE * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ctx->fft_buf == NULL) {
        ctx->fft_buf = calloc(YAMNET_FFT_SIZE * 2, sizeof(float));
    }
    ctx->magnitude = calloc(n_freqs, sizeof(float));
    ctx->mel_filters = calloc(YAMNET_MEL_BANDS * n_freqs, sizeof(float));
    ctx->frame_ring = heap_caps_calloc(1, sizeof(float) * YAMNET_PATCH_ELEMS, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ctx->frame_ring == NULL) {
        ctx->frame_ring = calloc(YAMNET_PATCH_ELEMS, sizeof(float));
    }
    ctx->pcm_cap = YAMNET_WINDOW_SAMPLES + YAMNET_HOP_SAMPLES * 4;
    ctx->pcm_ring = calloc(ctx->pcm_cap, sizeof(int16_t));

    if (!ctx->window || !ctx->fft_buf || !ctx->magnitude || !ctx->mel_filters ||
            !ctx->frame_ring || !ctx->pcm_ring) {
        yamnet_mel_destroy(ctx);
        return NULL;
    }

    /* Periodic Hann — matches tf.signal.stft / Google YAMNet training
     * (0.5 - 0.5*cos(2π i / N)), NOT the symmetric N-1 form. */
    for (int i = 0; i < YAMNET_WINDOW_SAMPLES; i++) {
        ctx->window[i] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * (float)i /
                                             (float)YAMNET_WINDOW_SAMPLES));
    }
    build_mel_filters(ctx->mel_filters);

    esp_err_t err = dsps_fft2r_init_fc32(NULL, YAMNET_FFT_SIZE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "dsps_fft2r_init_fc32 failed: %s", esp_err_to_name(err));
        yamnet_mel_destroy(ctx);
        return NULL;
    }
    ctx->fft_ok = true;
    ESP_LOGI(TAG, "mel frontend: HTK mel, |X| magnitude, log+0.001, periodic Hann");
    return ctx;
}

void yamnet_mel_destroy(yamnet_mel_ctx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }
    free(ctx->window);
    free(ctx->fft_buf);
    free(ctx->magnitude);
    free(ctx->mel_filters);
    free(ctx->frame_ring);
    free(ctx->pcm_ring);
    free(ctx);
}

static void compute_one_frame(yamnet_mel_ctx_t *ctx, const int16_t *pcm400, float *out_mel)
{
    const int n_fft = YAMNET_FFT_SIZE;
    const int n_freqs = n_fft / 2 + 1;

    memset(ctx->fft_buf, 0, sizeof(float) * n_fft * 2);
    for (int i = 0; i < YAMNET_WINDOW_SAMPLES; i++) {
        float s = ((float)pcm400[i] / 32768.0f) * ctx->window[i];
        ctx->fft_buf[2 * i] = s;
    }

    dsps_fft2r_fc32(ctx->fft_buf, n_fft);
    dsps_bit_rev_fc32(ctx->fft_buf, n_fft);
    dsps_cplx2reC_fc32(ctx->fft_buf, n_fft);

    for (int k = 0; k < n_freqs; k++) {
        float re = ctx->fft_buf[2 * k];
        float im = ctx->fft_buf[2 * k + 1];
        ctx->magnitude[k] = sqrtf(re * re + im * im);
    }

    for (int m = 0; m < YAMNET_MEL_BANDS; m++) {
        const float *row = &ctx->mel_filters[m * n_freqs];
        float sum = 0.0f;
        for (int k = 0; k < n_freqs; k++) {
            sum += row[k] * ctx->magnitude[k];
        }
        out_mel[m] = logf(sum + YAMNET_LOG_OFFSET);
    }
}

esp_err_t yamnet_mel_push_pcm(yamnet_mel_ctx_t *ctx, const int16_t *samples, size_t count)
{
    if (ctx == NULL || samples == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < count; i++) {
        if (ctx->pcm_len >= ctx->pcm_cap) {
            memmove(ctx->pcm_ring, ctx->pcm_ring + YAMNET_HOP_SAMPLES,
                    (ctx->pcm_len - YAMNET_HOP_SAMPLES) * sizeof(int16_t));
            ctx->pcm_len -= YAMNET_HOP_SAMPLES;
        }
        ctx->pcm_ring[ctx->pcm_len++] = samples[i];

        while (ctx->pcm_len >= YAMNET_WINDOW_SAMPLES) {
            float *dst = &ctx->frame_ring[(size_t)ctx->frame_write * YAMNET_MEL_BANDS];
            compute_one_frame(ctx, ctx->pcm_ring, dst);
            ctx->frame_write = (uint8_t)((ctx->frame_write + 1U) % YAMNET_PATCH_FRAMES);
            if (ctx->frame_count < YAMNET_PATCH_FRAMES) {
                ctx->frame_count++;
            }
            if (ctx->frames_since_export < 255) {
                ctx->frames_since_export++;
            }

            memmove(ctx->pcm_ring, ctx->pcm_ring + YAMNET_HOP_SAMPLES,
                    (ctx->pcm_len - YAMNET_HOP_SAMPLES) * sizeof(int16_t));
            ctx->pcm_len -= YAMNET_HOP_SAMPLES;
        }
    }
    return ESP_OK;
}

bool yamnet_mel_patch_ready(const yamnet_mel_ctx_t *ctx)
{
#ifndef CONFIG_YAMNET_INFER_HOP_FRAMES
#define CONFIG_YAMNET_INFER_HOP_FRAMES 48
#endif
    return ctx != NULL &&
           ctx->frame_count >= YAMNET_PATCH_FRAMES &&
           ctx->frames_since_export >= (uint8_t)CONFIG_YAMNET_INFER_HOP_FRAMES;
}

esp_err_t yamnet_mel_export_patch(yamnet_mel_ctx_t *ctx, float *out_patch, uint8_t hop_frames)
{
    if (ctx == NULL || out_patch == NULL || ctx->frame_count < YAMNET_PATCH_FRAMES) {
        return ESP_ERR_INVALID_STATE;
    }

    const uint8_t start = ctx->frame_write;
    for (int f = 0; f < YAMNET_PATCH_FRAMES; f++) {
        uint8_t idx = (uint8_t)((start + f) % YAMNET_PATCH_FRAMES);
        memcpy(&out_patch[f * YAMNET_MEL_BANDS],
               &ctx->frame_ring[idx * YAMNET_MEL_BANDS],
               sizeof(float) * YAMNET_MEL_BANDS);
    }

    ctx->frames_since_export = 0;
    (void)hop_frames;
    return ESP_OK;
}
