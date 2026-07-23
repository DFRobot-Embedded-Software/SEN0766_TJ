/**
 * @file mcp_provider.h
 * @brief MCP 传感器 Provider 接口（与传输层解耦，便于移植/扩展）
 */
#pragma once

#include "esp_err.h"
#include "esp_mcp_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 一块业务能力：注册 tools/resources，并可挂后台 runtime（watch/事件）。
 * mcp_core 不 include 任何具体传感器头文件。
 */
typedef struct mcp_sensor_provider {
    const char *name;
    esp_err_t (*register_tools)(esp_mcp_t *mcp);
    esp_err_t (*register_resources)(esp_mcp_t *mcp);
    esp_err_t (*start_runtime)(void);
    void (*stop_runtime)(void);
} mcp_sensor_provider_t;

#ifdef __cplusplus
}
#endif
