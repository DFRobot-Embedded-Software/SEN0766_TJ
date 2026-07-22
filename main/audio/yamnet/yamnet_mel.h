/**
 * @file yamnet_mel.h
 * @brief YAMNet log-mel 前端（96×64 patch）
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define YAMNET_SAMPLE_RATE     16000
#define YAMNET_WINDOW_SAMPLES  400   /* 25 ms */
#define YAMNET_HOP_SAMPLES     160   /* 10 ms */
#define YAMNET_FFT_SIZE        512
#define YAMNET_MEL_BANDS       64
#define YAMNET_PATCH_FRAMES    96
#define YAMNET_PATCH_ELEMS     (YAMNET_PATCH_FRAMES * YAMNET_MEL_BANDS)

typedef struct yamnet_mel_ctx yamnet_mel_ctx_t;

/** 分配 mel 上下文（PSRAM 优先）。 */
yamnet_mel_ctx_t *yamnet_mel_create(void);

/** 释放 mel 上下文。 */
void yamnet_mel_destroy(yamnet_mel_ctx_t *ctx);

/**
 * 推入单声道 int16 PCM；凑满一 hop 则追加一帧 mel。
 */
esp_err_t yamnet_mel_push_pcm(yamnet_mel_ctx_t *ctx, const int16_t *samples, size_t count);

/** 是否已缓冲至少 YAMNET_PATCH_FRAMES 帧，可导出 patch。 */
bool yamnet_mel_patch_ready(const yamnet_mel_ctx_t *ctx);

/**
 * 拷贝最近 96 帧到 out_patch（行主序 float log-mel），再前进 hop_frames（常用 48）。
 */
esp_err_t yamnet_mel_export_patch(yamnet_mel_ctx_t *ctx,
                                  float *out_patch,
                                  uint8_t hop_frames);

#ifdef __cplusplus
}
#endif
