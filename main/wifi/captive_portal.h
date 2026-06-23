/**
 * @file captive_portal.h
 * @brief AP 配网模式下的 Captive Portal（DNS 劫持 + 探测 URL 重定向）
 */

#pragma once

#include "esp_err.h"
#include "esp_http_server.h"
#include <stdbool.h>

/** @brief 启动 DNS 服务器，将所有域名解析到 192.168.4.1 */
esp_err_t captive_portal_start(void);

/** @brief 停止 DNS 服务器 */
void captive_portal_stop(void);

/** @brief 是否为 Captive Portal 探测 URL */
bool captive_portal_is_probe_uri(const char *uri);

/** @brief 发送 Captive Portal 重定向响应 */
esp_err_t captive_portal_redirect(httpd_req_t *req);
