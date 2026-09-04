/**
 * @file comm_hub.h
 * @brief 入网后启动设备通道。固件不跑 MCP。
 *
 * 枢纽：mDNS → CoAP → MQTT 长连接 → 语音 WS 发现。
 * 叶子 CONFIG_COMM_ROLE_LEAF：不启 CoAP/mDNS/WS；MQTT 仅 publish 时短连。
 * 配网劫持不在这里（captive_portal，入网后必须 stop）。
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t comm_hub_start(void);
void comm_hub_stop(void);

#ifdef __cplusplus
}
#endif
