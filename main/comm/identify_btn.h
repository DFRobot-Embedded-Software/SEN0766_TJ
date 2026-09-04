/**
 * @file identify_btn.h
 * @brief 天花板维护：长按打开临时 APSTA，不靠串口。
 *
 * 行为：按住 CONFIG_COMM_IDENTIFY_HOLD_MS → 开 SSID MCP-Sensors-Find（开放），
 *       门户 302 到 http://192.168.4.1/identify，页上写当前 STA IP、id、mcp-sensors-<id>.local。
 *       再按或 CONFIG_COMM_IDENTIFY_AP_S 超时关 AP；STA 业务尽量保持。
 * 方向：人 → 手机连 AP → HTTP。不是传感器协议，禁止在页上用 WS/MQTT 传业务。
 */
#pragma once
#include "esp_err.h"
esp_err_t identify_btn_start(void);
