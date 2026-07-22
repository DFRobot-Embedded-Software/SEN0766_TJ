/**
 * @file audio_stream.h
 * @brief 唤醒后 UDP 裸 PCM 网络透传（与 YAMNet 解耦）
 *
 * 自行订阅 audio_capture；仅在 audio_stream_start() 之后向网络发送。
 * 正式触发点留给 esp-sr 唤醒回调。
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 初始化透传：acquire 采集、注册 sink、创建发送任务。
 * 未开启 CONFIG_AUDIO_STREAM_ENABLE 时为空操作并返回 ESP_OK。
 */
esp_err_t audio_stream_init(void);

/**
 * 反初始化：停止发送、注销 sink、release 采集。
 */
void audio_stream_deinit(void);

/**
 * 开始 UDP 发送，持续至 stop 或达到 AUDIO_STREAM_MAX_MS。
 */
esp_err_t audio_stream_start(void);

/**
 * 立即停止 UDP 发送并关闭套接字。
 */
void audio_stream_stop(void);

/**
 * 是否处于发送中。
 */
bool audio_stream_is_active(void);

/**
 * 采集 sink 入口：仅在 streaming 时入队；队列满则丢旧保实时。
 * 也可由内部注册；一般无需业务层直接调用。
 */
void audio_stream_on_pcm(const int16_t *pcm, size_t n_samples);

#ifdef __cplusplus
}
#endif
