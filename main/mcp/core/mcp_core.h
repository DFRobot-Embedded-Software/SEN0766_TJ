/**
 * @file mcp_core.h
 * @brief MCP 传输与生命周期（可移植；不含具体传感器）
 */
#pragma once

#include "esp_err.h"
#include "esp_mcp_engine.h"
#include "mcp_provider.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef MCP_HTTP_PORT
#define MCP_HTTP_PORT 8080
#endif

#ifndef MCP_ENDPOINT_NAME
#define MCP_ENDPOINT_NAME "mcp_server"
#endif

#define MCP_ENDPOINT_URI "/" MCP_ENDPOINT_NAME

#define MCP_CORE_MAX_PROVIDERS 8

esp_err_t mcp_core_add_provider(const mcp_sensor_provider_t *provider);

/**
 * 启动：create → 各 provider 注册 tools/resources → HTTP mgr → start_runtime
 */
esp_err_t mcp_core_start(void);

void mcp_core_stop(void);

bool mcp_core_is_running(void);

/** 向订阅者推送 resources/updated（URI 字符串） */
void mcp_core_notify_resource(const char *uri);

esp_mcp_t *mcp_core_get_mcp(void);

#ifdef __cplusplus
}
#endif
