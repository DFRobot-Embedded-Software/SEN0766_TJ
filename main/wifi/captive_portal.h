/**
 * @file captive_portal.h
 * @brief 仅 SoftAP（配网 MCP-Sensors-Setup 或查地址 MCP-Sensors-Find）：
 *        UDP/53 DNS 劫持 + UDP/5353 mDNS 劫持，把任意主机名指到 192.168.4.1，
 *        HTTP 探测 URL 再 302 到门户。
 *
 * 方向：手机 → 设备 AP。入网 STA 后必须 captive_portal_stop()，禁止继续抢局域网 .local。
 * 业务 mDNS（_mcp-sensors）是「宣告自己」，和本劫持不是一回事。
 */
#pragma once

#include "esp_err.h"
#include "esp_http_server.h"
#include <stdbool.h>

esp_err_t captive_portal_start(void);
void captive_portal_stop(void);
bool captive_portal_is_probe_uri(const char *uri);
esp_err_t captive_portal_redirect(httpd_req_t *req);

/** 302 Location，默认 http://192.168.4.1/ ；查地址改为 .../identify */
void captive_portal_set_redirect(const char *url);
