/**
 * @file device_id.h
 * @brief 短 id：STA/熔丝 MAC 的后 3 字节，6 位小写 hex（例 a1b2c3）。
 *
 * 贯穿（必须同一字符串，禁止再生成第二套）：
 *   mDNS TXT id、主机名 mcp-sensors-<id>
 *   MQTT mcp_sensors/<id>/event
 *   HA config unique_id / 实体 unique_id 前缀
 *   脚本工具 {id}__sensors.get_snapshot
 *
 * 入网前即可算（配网成功页要写出 http://mcp-sensors-<id>.local）。
 * 禁止把温度等业务放进 id；换 MCU 仍用网卡 MAC 同一规则。
 */
#pragma once

#include "esp_err.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 写出 6 位小写 hex，out_len >= 7（含 '\0'） */
esp_err_t device_id_get(char *out, size_t out_len);

/** 写出 mcp-sensors-<id>，out_len 建议 >= 24 */
esp_err_t device_id_hostname(char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
