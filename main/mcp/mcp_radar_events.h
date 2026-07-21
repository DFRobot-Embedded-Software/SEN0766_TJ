/**
 * @file mcp_radar_events.h
 * @brief C4004 雷达事件检测（动态区域，供 MCP 资源订阅）
 */

#pragma once

#include "esp_err.h"
#include <stdint.h>

/** 全局事件流：含当前 areas 列表 + 近期 events（随 Web 配置动态变化） */
#define MCP_RESOURCE_EVENTS_URI "sensor://events"

/** 单区域事件 URI 格式，id 与 area_config.tags 下标一致 */
#define MCP_RESOURCE_AREA_EVENTS_URI_FMT "sensor://area/%u/events"

/** area_id 为 room 级事件（无单独区域资源） */
#define MCP_EVENT_AREA_ID_GLOBAL 255

typedef void (*mcp_radar_event_notify_cb)(uint8_t area_id);

esp_err_t mcp_radar_events_start(mcp_radar_event_notify_cb notify_cb);
void mcp_radar_events_stop(void);

/** 区域增删改后重置检测状态（避免下标错位） */
void mcp_radar_events_reset_state(void);

/** 清空事件环形缓冲区（区域删除后避免 read 仍返回旧区域事件） */
void mcp_radar_events_clear_log(void);

/** 读取全局事件流 JSON（调用方 free） */
esp_err_t mcp_radar_events_read_feed(char **out_json);

/** 读取某区域事件 JSON（调用方 free）；area_id 超出当前配置返回 ESP_ERR_NOT_FOUND */
esp_err_t mcp_radar_events_read_area(uint8_t area_id, char **out_json);
