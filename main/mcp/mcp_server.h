/**
 * @file mcp_server.h
 * @brief 板级 MCP 入口（薄封装 mcp_core + Providers）
 *
 * Web Dashboard :80；MCP :8080 /mcp_server
 */
#pragma once

#include "esp_err.h"
#include "mcp_core.h"
#include "mcp_provider_env.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t mcp_server_start(void);
void mcp_server_stop(void);

#ifdef __cplusplus
}
#endif
