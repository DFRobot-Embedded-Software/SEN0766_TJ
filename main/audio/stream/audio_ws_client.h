/**
 * @file audio_ws_client.h
 * @brief 设备 = WebSocket 客户端，只音频。对端可以是 voice-bridge 或 HA 内同类 server。
 *
 * 二进制：PCM 16 kHz s16le 单声道。
 * 文本会话 JSON（op 字段）：
 *   设备→对端  uplink_start {rate:16000,format:s16le,channels:1}  随后发 PCM
 *   设备→对端  dialog_idle  本轮结束，下一轮需再唤醒
 *   对端→设备  play_stop    下行播完，排空 ring 后保持连接
 * 禁止：在 WS 上传 snapshot、改阈值、雷达坐标、MQTT 事件。哭声走 MQTT。
 *
 * 未开 CONFIG_AUDIO_WS_ENABLE 时为空操作（无麦板裁掉即可）。
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
 * 续听窗口（followup armed）不算 busy。
 */
bool audio_ws_client_is_busy(void);

/**
 * TTS 播完后的续听窗口是否激活（可凭 VAD 直接开上行，无需唤醒）。
 * CONFIG_AUDIO_WS_FOLLOWUP_MS==0 时恒为 false。
 */
bool audio_ws_client_is_followup_armed(void);

/**
 * 续听窗口内检测到语音，或窗口内再次唤醒时调用：复用连接开上行。
 * 与 session_start 相同排队路径。
 */
esp_err_t audio_ws_client_followup_begin(void);

/**
 * 上行已开但长时间未听到有效语音：停上行、发 dialog_idle、结束对话窗口。
 * 不进入 await-downlink。
 */
void audio_ws_client_followup_nospeech_abort(void);

/** AFE 已听到有效语音：取消空上行短超时。 */
void audio_ws_client_notify_speech_heard(void);

/** 采集 sink（一般由模块自行注册）。 */
void audio_ws_client_on_pcm(const int16_t *pcm, size_t n_samples, void *ctx);

#ifdef __cplusplus
}
#endif
