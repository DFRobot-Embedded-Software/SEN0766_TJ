/**
 * @file mcp_server.h
 * @brief MCP HTTP 服务（esp_mcp_transport_http_server，独立 8080 端口）
 *
 * Web Dashboard 与 MCP 使用两个独立 httpd：
 * - Web：80 端口（web_server.c）
 * - MCP：8080 端口，Streamable HTTP 端点 /mcp_server
 *
 * Hermes 等客户端配置示例：
 *   url: "http://<device-ip>:8080/mcp_server"
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>

/** MCP 专用 HTTP 端口（与 Web 80 分离，避免路由冲突） */
#define MCP_HTTP_PORT        8080

/** MCP Streamable HTTP 端点路径名（完整 URI 为 /mcp_server） */
#define MCP_ENDPOINT_NAME    "mcp_server"
#define MCP_ENDPOINT_URI     "/" MCP_ENDPOINT_NAME

/** 可订阅的温度资源 URI（resources/subscribe + SSE 推送） */
#define MCP_RESOURCE_TEMPERATURE_URI "sensor://temperature"

/** 通知订阅者某资源已更新（供 mcp_radar_events 等模块调用） */
void mcp_server_notify_resource(const char *uri);

/** 启动 MCP 服务（幂等，重复调用返回 ESP_OK） */
esp_err_t mcp_server_start(void);

/** 停止 MCP 服务并释放 httpd / 引擎资源 */
void mcp_server_stop(void);

bool mcp_server_is_running(void);
