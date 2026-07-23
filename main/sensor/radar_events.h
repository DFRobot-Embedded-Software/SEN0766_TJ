/**
 * @file radar_events.h
 * @brief C4004 区域事件检测（纯领域逻辑，不依赖 MCP 传输）
 *
 * 事件写入环形缓冲区，read 时附带当前 area_config 中的区域列表。
 * 用户通过 Web 增删区域后无需改代码，调用方（如 mcp_provider_radar）
 * 负责把这里的通知转换成协议层的资源更新。
 */
#pragma once

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** area_id 为 room 级事件（无单独区域资源） */
#define RADAR_EVENT_AREA_ID_GLOBAL 255

typedef void (*radar_event_notify_cb)(uint8_t area_id);

esp_err_t radar_events_start(radar_event_notify_cb notify_cb);
void radar_events_stop(void);

/** 区域增删改后重置检测状态（避免下标错位） */
void radar_events_reset_state(void);

/** 清空事件环形缓冲区（区域删除后避免 read 仍返回旧区域事件） */
void radar_events_clear_log(void);

/** 读取全局事件流 JSON（调用方 free） */
esp_err_t radar_events_read_feed(char **out_json);

/** 读取某区域事件 JSON（调用方 free）；area_id 超出当前配置返回 ESP_ERR_NOT_FOUND */
esp_err_t radar_events_read_area(uint8_t area_id, char **out_json);

#ifdef __cplusplus
}
#endif
