/**
 * @file voice_discover.h
 * @brief 设备作为 WS 客户端，只解析「音频对端 URI」。不发现传感器、不配 CoAP。
 *
 * 浏览：
 *   _voice-ws._tcp  TXT ws=  → 音频对端
 *   _mcp-gw._tcp    TXT mqtt=（非 127.0.0.1）→ mqtt_discover；TXT ws= 作音频回退
 *
 * 会话 JSON / PCM 见 audio_ws_client.h。禁止在 TXT 里放快照。
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t voice_discover_start(void);
void voice_discover_stop(void);
/** 空字符串表示尚未发现，调用方回退 Kconfig */
const char *voice_discover_get_ws_uri(void);

#ifdef __cplusplus
}
#endif
