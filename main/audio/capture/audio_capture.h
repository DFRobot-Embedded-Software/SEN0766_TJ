/**
 * @file audio_capture.h
 * @brief 独立音频采集层：ES8388/I2S + 数字增益 + 多路 PCM 订阅
 *
 * 唯一允许打开麦克风硬件的模块。YAMNet / esp-sr / 网络透传只通过 sink 订阅，
 * 不得直接操作 codec。
 */
#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_CAPTURE_SAMPLE_RATE 16000
#define AUDIO_CAPTURE_MAX_SINKS   4

/**
 * PCM 消费者回调（采集任务内、数字增益之后调用）。
 * @param pcm        单声道 int16 样本
 * @param n_samples  样本数
 * @param ctx        注册时传入的用户上下文
 */
typedef void (*audio_pcm_sink_fn)(const int16_t *pcm, size_t n_samples, void *ctx);

/**
 * 占用采集：引用计数 +1。首次调用时打开 ES8388 并启动采集任务。
 * @return ESP_OK 或硬件/内存错误码
 */
esp_err_t audio_capture_acquire(void);

/**
 * 释放采集：引用计数 -1。归零时关闭硬件与采集任务。
 */
void audio_capture_release(void);

/**
 * 注册一路 PCM 订阅（最多 AUDIO_CAPTURE_MAX_SINKS 路）。
 * @param fn      回调，不可为 NULL
 * @param ctx     透传给回调
 * @param out_id  可选，返回 sink 编号供 remove 使用
 */
esp_err_t audio_capture_add_sink(audio_pcm_sink_fn fn, void *ctx, int *out_id);

/**
 * 注销指定 sink（id 由 add_sink 返回）。
 */
void audio_capture_remove_sink(int id);

/**
 * 当前采样率（Hz），固定为 AUDIO_CAPTURE_SAMPLE_RATE。
 */
uint32_t audio_capture_sample_rate(void);

#ifdef __cplusplus
}
#endif
