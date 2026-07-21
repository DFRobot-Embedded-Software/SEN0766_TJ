/**
 * @file web_server.h
 * @brief HTTP Web 服务器模块（STA 模式 Dashboard，端口 80）
 */

#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

esp_err_t web_server_start_provision(void);
esp_err_t web_server_start_dashboard(void);
void web_server_stop(void);
httpd_handle_t web_server_get_handle(void);
esp_err_t web_server_register_static_routes(void);
