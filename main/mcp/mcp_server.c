/**
 * @file mcp_server.c
 * @brief 板级 MCP 入口：注册 Providers 并启动 mcp_core
 *
 * 传输与传感器逻辑已拆到 mcp/core 与 mcp/providers；本文件只负责
 * "这块板子有哪些 Provider"，新增传感器只需在此追加注册。
 */
#include "mcp_server.h"

#include "mcp_core.h"
#include "mcp_provider_env.h"
#include "mcp_provider_radar.h"

#include "esp_log.h"

static const char *TAG = "mcp_server";

esp_err_t mcp_server_start(void)
{
    if (mcp_core_is_running()) {
        return ESP_OK;
    }

    ESP_ERROR_CHECK(mcp_core_add_provider(&mcp_provider_env));
    ESP_ERROR_CHECK(mcp_core_add_provider(&mcp_provider_radar));

    esp_err_t err = mcp_core_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mcp_core_start failed: %s", esp_err_to_name(err));
    }
    return err;
}

void mcp_server_stop(void)
{
    mcp_core_stop();
}
