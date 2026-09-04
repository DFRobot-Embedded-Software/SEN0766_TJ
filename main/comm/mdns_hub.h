/**
 * @file mdns_hub.h
 * @brief 入网后只宣告「我是谁、CoAP 在哪」。不是配网，也不塞传感器读数。
 *
 * 方向：设备 → 局域网组播。HA zeroconf 与 mcp_gw 浏览同一服务，用 TXT id 去重。
 *
 * 主机名：mcp-sensors-<id>.local（id 见 device_id.h）。Android 上 .local 不可靠，
 *         查 IP 仍靠临时 AP /identify，不能把主机名当唯一手段。
 *
 * 服务：
 *   _http._tcp :80
 *     浏览器仪表盘 / OTA。入网后不再劫持别人的 .local。
 *   _mcp-sensors._udp :5683
 *     HA / 脚本发现 CoAP。不要改成裸 _coap._udp（避免和别的 CoAP 设备混在一起）。
 *     TXT（仅这三项，禁止塞 temperature 等）：
 *       id    6 位 hex，与 MQTT topic、HA unique_id、工具前缀相同
 *       ver   契约版本，目前 "1"
 *       name  显示名，如 "MCP Sensors abcdef"
 *
 * 禁止：配网 SoftAP 期间用本模块抢全网 .local（那是 captive_portal 的 mDNS 劫持，且必须在入网后 stop）。
 *       取消旧服务名 _mcp。在线与否不单靠组播，靠 CoAP 探活和/或 MQTT LWT。
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t mdns_hub_start(void);
void mdns_hub_stop(void);

#ifdef __cplusplus
}
#endif
