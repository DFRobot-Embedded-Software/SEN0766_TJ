/**
 * @file rgb_ui.h
 * @brief RGB 灯效：语音三态 > 配网 > 日常
 *
 * 语音仅三种状态：识别 / 处理 / 播放。
 */
#pragma once

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RGB_NET_NONE = 0,
    RGB_NET_SELFTEST,
    RGB_NET_AP_WAIT,
    RGB_NET_CONNECTING,
    RGB_NET_OK,
    RGB_NET_FAIL,
} rgb_net_t;

/** 语音会话灯效（仅三态 + 空闲） */
typedef enum {
    RGB_VOICE_NONE = 0,   /**< 空闲，回到配网/日常层 */
    RGB_VOICE_LISTEN,     /**< 识别音频：上行拾音 / ASR */
    RGB_VOICE_PROCESS,    /**< 处理音频：等 Hermes / TTS 合成 */
    RGB_VOICE_PLAY,       /**< 播放音频：下行 TTS */
} rgb_voice_t;

esp_err_t rgb_ui_init(void);
void rgb_ui_set_net(rgb_net_t net);
void rgb_ui_set_voice(rgb_voice_t voice);
rgb_net_t rgb_ui_get_net(void);
rgb_voice_t rgb_ui_get_voice(void);

#ifdef __cplusplus
}
#endif
