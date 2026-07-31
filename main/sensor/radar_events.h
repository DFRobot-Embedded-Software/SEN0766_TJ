/**
 * @file radar_events.h
 * @brief C4004 毫米波事件检测器（写入统一 device_event_bus）
 *
 * 检测逻辑与传输解耦；MCP/Web 从总线读同一份日志。
 * notify_cb 可选（兼容旧路径）；推荐挂 device_event_subscribe。
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

struct cJSON;

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

/**
 * 填充 Web「最近事件」：优先本模块缓存的 range_enter/exit（不依赖总线是否被清空）
 * out_array 须为已创建的 cJSON 数组；按旧→新追加，最多 max_count 条
 */
esp_err_t radar_events_fill_web_recent(struct cJSON *out_array, int max_count);

/** 当前是否判定检测范围有人 */
bool radar_events_range_occupied(void);

#ifdef __cplusplus
}
#endif
