/**
 * @file mcp_provider_radar.h
 */
#pragma once

#include "mcp_provider.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const mcp_sensor_provider_t mcp_provider_radar;

/** 全局事件流：含当前 areas 列表 + 近期 events（随 Web 配置动态变化） */
#define MCP_RESOURCE_EVENTS_URI "sensor://events"

/** 单区域事件 URI 格式，id 与 area_config.tags 下标一致 */
#define MCP_RESOURCE_AREA_EVENTS_URI_FMT "sensor://area/%u/events"

#ifdef __cplusplus
}
#endif
