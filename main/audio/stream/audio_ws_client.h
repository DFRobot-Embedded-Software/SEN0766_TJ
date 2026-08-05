/**
 * @file audio_ws_client.h
 * @brief 设备作为 WebSocket 客户端：唤醒后上行 PCM，接收下行并播放
 *
 * 协议见 README_WS.md。未开启 CONFIG_AUDIO_WS_ENABLE 时为空操作。
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t audio_ws_client_init(void);
void audio_ws_client_deinit(void);

/**
 * 开始一次拾音会话：连接网关、发送 uplink_start、开始上行 PCM。
 * 可在唤醒回调 / fetch 任务中调用（内部排队到 WS 任务）。
 */
esp_err_t audio_ws_client_session_start(void);

/**
 * 结束上行并断开（发送 uplink_stop）。
 * @param reason "vad" | "max" | "error" | "manual"
 */
void audio_ws_client_session_stop(const char *reason);

/** AFE 判定停说后调用（轻量，可从 fetch 任务调用）。 */
void audio_ws_client_notify_eos(void);

/** 是否正在上行拾音。 */
bool audio_ws_client_is_uplink_active(void);

/** 是否正在播放下行。 */
bool audio_ws_client_is_playing(void);

/**
 * 会话忙碌：上行 / 等下行 / 播放 / 已连接未收尾。
 * 唤醒侧应用此判断，避免中途重连打断。
 */
bool audio_ws_client_is_busy(void);

/** 采集 sink（一般由模块自行注册）。 */
void audio_ws_client_on_pcm(const int16_t *pcm, size_t n_samples, void *ctx);

#ifdef __cplusplus
}
#endif
