/**
 * @file rgb_ui.h
 * @brief RGB 灯效：语音 > 配网 > 日常
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

typedef enum {
    RGB_VOICE_NONE = 0,
    RGB_VOICE_WAKE_OK,
    RGB_VOICE_LISTEN,
    RGB_VOICE_PARSE,
    RGB_VOICE_CMD_OK,
    RGB_VOICE_CMD_FAIL,
    RGB_VOICE_TTS,
    RGB_VOICE_TIMEOUT,
} rgb_voice_t;

esp_err_t rgb_ui_init(void);
void rgb_ui_set_net(rgb_net_t net);
void rgb_ui_set_voice(rgb_voice_t voice);
void rgb_ui_tts_level(uint8_t brightness); /**< 仅 TTS 阶段有效 */
rgb_net_t rgb_ui_get_net(void);

#ifdef __cplusplus
}
#endif
