/**
 * @file mcp_provider_env.h
 */
#pragma once

#include "mcp_provider.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const mcp_sensor_provider_t mcp_provider_env;

#define MCP_RESOURCE_TEMPERATURE_URI "sensor://temperature"

#ifdef __cplusplus
}
#endif
