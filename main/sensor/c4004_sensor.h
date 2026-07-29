/**
 * @file c4004_sensor.h
 * @brief DFRobot C4004 毫米波雷达 (UART)
 */

#pragma once

#include "esp_err.h"
#include "radar_area.h"
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

/** @brief 主动刷新轨迹目标列表（供 snapshot 与人数统计交叉校验） */
esp_err_t c4004_sensor_refresh_targets(void);

/**
 * @brief 用轨迹修正 strategy 1 人数
 * TAG 多于轨迹时保留 TAG（多目标）；TAG 少于轨迹时用轨迹压虚高
 * @return 是否修正了显示值
 */
bool c4004_sensor_refine_people_count(uint8_t index, const area_rect_t *rect, area_status_t *st);

/**
 * @brief 全量恢复 C4004 区域状态（等效于模块重新上电后的标签同步）
 * @param full_reset true=清空雷达全部标签后重建；false=仅 re-sync 当前配置
 */
esp_err_t c4004_sensor_recover(const area_config_t *cfg, bool full_reset);

/**
 * @brief 统计矩形区域内的目标人数（按 kinesia 区分活动/静止）
 * @param rect 区域矩形 (cm)，与 area_coords_to_rect 输出一致
 */
esp_err_t c4004_sensor_count_targets_in_rect(const area_rect_t *rect, int *active, int *static_count);

/**
 * @brief 按当前传感器设置与加速度倾角，重新下发安装模式/高度/角度到 C4004
 */
esp_err_t c4004_sensor_sync_install(void);

/** @brief 轨迹目标快照（厘米，雷达坐标系） */
typedef struct {
    uint8_t index;
    uint8_t kinesia;
    int16_t x_cm;
    int16_t y_cm;
    int16_t speed_cm_s;
    bool moving;
} c4004_target_info_t;

/**
 * @brief 复制最近一次 query/refresh 缓存的全部轨迹目标
 * @note 通常先调用 c4004_sensor_query_target / refresh_targets
 */
esp_err_t c4004_sensor_get_targets(c4004_target_info_t *out, uint8_t max_count, uint8_t *out_count);

/** @brief 当前检测 FOV（厘米），供网页按雷达坐标系投影 */
void c4004_sensor_get_fov_cm(int16_t *x_min, int16_t *x_max, int16_t *y_min, int16_t *y_max);

