/**
 * @file mqtt_discover.h
 * @brief 保存 broker，不浏览设备、不浏览 _mqtt._tcp。
 *
 * 主来源：voice_discover 扫到 _mcp-gw._tcp 的 TXT mqtt=
 * 兜底来源：网关/HA HTTP POST /api/wifi action=mqtt
 * （可选 mqtt_user / mqtt_pass）。127.0.0.1 一律丢弃。
 * 上次合法 TXT 写入 NVS，Wi‑Fi 重连时先用，再等新 TXT。
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t mqtt_discover_start(void);
void mqtt_discover_stop(void);
esp_err_t mqtt_discover_refresh(void);
/** 由 voice_discover（TXT）或 HTTP API（action=mqtt）调用 */
esp_err_t mqtt_discover_apply(const char *uri, const char *user, const char *pass);
const char *mqtt_discover_get_broker_uri(void);
const char *mqtt_discover_get_username(void);
const char *mqtt_discover_get_password(void);

#ifdef __cplusplus
}
#endif
