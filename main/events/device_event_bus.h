/**
 * @file device_event_bus.h
 * @brief 统一设备事件总线：环形日志 + 订阅通知（MCP / Web 共用）
 */
#pragma once

#include "device_event_types.h"
#include "esp_err.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct cJSON;

typedef struct {
    const char *type;       /**< 必填，如 area_enter */
    const char *category;   /**< 必填，默认 mmwave */
    uint8_t area_id;        /**< DEVICE_EVENT_AREA_ID_GLOBAL 表示全局 */
    const char *area;       /**< 区域名，可 NULL */
    int person_id;          /**< 轨迹 index，<0 表示省略 */
    int active;             /**< <0 省略 */
    int static_count;       /**< <0 省略 */
    const char *message;    /**< 可读描述，可 NULL */
    const struct cJSON *payload; /**< 可选附加对象，emit 时深拷贝进 JSON */
} device_event_t;

typedef void (*device_event_notify_fn)(const char *type, uint8_t area_id, void *ctx);

esp_err_t device_event_bus_init(void);

/** 写入环形日志并通知订阅者；内部复制字段，调用方可立即释放临时缓冲 */
esp_err_t device_event_emit(const device_event_t *ev);

esp_err_t device_event_subscribe(device_event_notify_fn fn, void *ctx);

/** 清空日志 */
void device_event_clear(void);

/**
 * 读取全局事件流 JSON（调用方 free）
 * category_filter 非 NULL 时仅返回该 category；传 NULL 返回全部
 */
esp_err_t device_event_read_feed(char **out_json);
esp_err_t device_event_read_feed_filtered(const char *category_filter, char **out_json);

/**
 * 读取某区域事件 JSON（本区域 + room_empty/range）
 * area_id 超出配置返回 ESP_ERR_NOT_FOUND
 */
esp_err_t device_event_read_area(uint8_t area_id, char **out_json);

/**
 * 取最新一条 mmwave 事件的精简对象（供 /api/data last_event）
 */
esp_err_t device_event_read_latest_mmwave(char **out_json);

/**
 * 取最新一条指定 category 的事件对象 JSON（调用方 free）
 * 无事件返回 ESP_ERR_NOT_FOUND
 */
esp_err_t device_event_read_latest_by_category(const char *category, char **out_json);

/**
 * 将最近 max_count 条（时间从旧到新）指定 category 事件追加到 out_array
 * category 为 NULL 时不过滤；max_count<=0 表示全部
 */
esp_err_t device_event_fill_recent(const char *category, struct cJSON *out_array, int max_count);

/** 扫描日志：该区域是否有未配对的 area_enter */
bool device_event_area_has_unpaired_enter(uint8_t area_id);

#ifdef __cplusplus
}
#endif
