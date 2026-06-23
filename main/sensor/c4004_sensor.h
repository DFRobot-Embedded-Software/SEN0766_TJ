/**
 * @file c4004_sensor.h
 * @brief DFRobot C4004 毫米波雷达 (UART)
 */

#pragma once

#include "esp_err.h"
#include "sensor_data.h"
#include <stdbool.h>
#include <stdint.h>

/** @brief 初始化 UART 并启动后台解析任务 */
esp_err_t c4004_sensor_start(void);

/** @brief 雷达是否已成功连接 */
bool c4004_sensor_is_ready(void);

/**
 * @brief 将 Web/NVS 区域标签同步写入 C4004
 * @param prev_count 同步前的标签数：0=首次/全量重建；大于 cfg->tag_count=有删除
 */
esp_err_t c4004_sensor_sync_tags(const area_config_t *cfg, uint8_t prev_count);

/** @brief 读取各标签最新状态（按 tag 下标，与 area_config.tags 对应） */
esp_err_t c4004_sensor_get_tag_status(uint8_t index, area_status_t *out);

/** @brief 主动查询当前轨迹目标；多目标时优先返回运动目标坐标 (cm) */
esp_err_t c4004_sensor_query_target(float *x, float *y, uint8_t *target_count, int8_t *picked_index);
