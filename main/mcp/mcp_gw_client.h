/**
 * @file mcp_gw_client.h
 * @brief 向局域网聚合网关 (mcp_gw) 注册本机 MCP，并周期性心跳
 */
#pragma once

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 本机设备短 id（MAC 后 3 字节十六进制，小写），缓冲至少 7 字节 */
esp_err_t mcp_gw_device_id(char *out, size_t out_len);

/**
 * mDNS/回退后解析到的语音 WebSocket URI（如 ws://192.168.1.27:8765/ws）。
 * 未发现时返回空字符串（非 NULL）。
 */
const char *mcp_gw_get_voice_ws_uri(void);

/**
 * WiFi 已连接且本机 MCP 已启动后调用。
 * @param local_ip 本机 IPv4 字符串
 */
esp_err_t mcp_gw_client_start(const char *local_ip);

void mcp_gw_client_stop(void);

#ifdef __cplusplus
}
#endif
