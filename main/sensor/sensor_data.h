/**
 * @file sensor_data.h
 * @brief 传感器数据与 C4004 区域标签配置
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#define AREA_TAG_MAX       16
#define AREA_TAG_NAME_LEN  32

/** @brief C4004 区域标签 */
typedef struct {
    uint8_t strategy;
    char name[AREA_TAG_NAME_LEN];
    float coords[4][2];
} area_tag_t;

/** @brief 区域配置（NVS 持久化） */
typedef struct {
    bool enabled;
    uint8_t tag_count;
    area_tag_t tags[AREA_TAG_MAX];
} area_config_t;

/** @brief 单个区域实时状态 */
typedef struct {
    char name[AREA_TAG_NAME_LEN];
    uint8_t strategy;
    int active;
    int static_count;
    bool enter;
    bool exit;
    bool approaching;
    bool leaving;
} area_status_t;

/** @brief 传感器快照，对应 GET /api/data */
typedef struct {
    bool env_ready;
    float temperature;
    float humidity;
    int co2;
    uint8_t area_count;
    area_status_t areas[AREA_TAG_MAX];
} sensor_snapshot_t;

/** @brief Web 四点坐标换算后的 C4004 矩形区域 (cm) */
typedef struct {
    float min_x;
    float min_y;
    float max_x;
    float max_y;
    int16_t center_x;
    int16_t center_y;
    uint16_t width;
    uint16_t height;
} area_rect_t;

#define AREA_RECT_MIN_SIZE_CM 10

void sensor_data_init(void);
esp_err_t sensor_data_get_snapshot(sensor_snapshot_t *out);

/** @brief 从 C4004 读取当前目标坐标 (cm)，供 Web 采集角点；picked_index 可传 NULL */
esp_err_t sensor_data_fetch_radar_coord(float *x, float *y, uint8_t *target_count, int8_t *picked_index);

/** @brief 校验 4 点坐标是否构成有效矩形 */
bool area_coords_valid(const area_tag_t *tag);

/** @brief 由 4 角点计算中心与宽高，并输出调试日志 */
esp_err_t area_coords_to_rect(const area_tag_t *tag, area_rect_t *out);

/** @brief 按策略调整矩形（边缘门使用窄条检测带） */
esp_err_t area_coords_to_rect_for_radar(const area_tag_t *tag, area_rect_t *out);

esp_err_t area_config_get(area_config_t *out);
esp_err_t area_config_set_enabled(bool enabled);
esp_err_t area_config_add_tag(const area_tag_t *tag);
esp_err_t area_config_remove_tag(uint8_t index);

/** 区域配置变更回调（增删标签、开关雷达区域后触发） */
typedef void (*area_config_changed_cb_t)(void);
void area_config_set_change_callback(area_config_changed_cb_t cb);
