/**
 * @file mcp_json_util.h
 * @brief MCP 工具返回值辅助（cJSON → esp_mcp_value）
 */
#pragma once

#include "cJSON.h"
#include "esp_mcp_tool.h"
#include <stdlib.h>

static inline esp_mcp_value_t mcp_json_to_value(cJSON *json)
{
    if (json == NULL) {
        return esp_mcp_value_create_bool(false);
    }

    char *payload = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (payload == NULL) {
        return esp_mcp_value_create_bool(false);
    }

    esp_mcp_value_t result = esp_mcp_value_create_string(payload);
    free(payload);
    if (result.type == ESP_MCP_VALUE_TYPE_INVALID) {
        return esp_mcp_value_create_bool(false);
    }
    return result;
}
