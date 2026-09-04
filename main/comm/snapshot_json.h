/**
 * @file snapshot_json.h
 * @brief CoAP GET /sensors/snapshot 的 JSON 对象。只描述「此刻」，不推变化。
 *
 * 方向：设备 → Client（问答）。进门/哭声/超限变化走 MQTT event。
 * 主要字段：
 *   env_ready, temperature_c, humidity_pct, co2_ppm
 *   lux_ready, lux
 *   areas[]：name, strategy，以及 enter/exit 或 approaching/leaving 或 active/static
 *   sound_ready, sound_db, sound_warning_db, sound_alarm
 *   thresholds：与 GET /sensors/thresholds 同形
 * 禁止：在本对象里塞 PCM、会话 op、MQTT topic。
 */
#pragma once

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 调用方 cJSON_Delete；失败返回 NULL */
cJSON *snapshot_json_build(void);

#ifdef __cplusplus
}
#endif
