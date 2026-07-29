/**
 * @file env_sensors.h
 * @brief 环境传感器聚合（STCC4 温湿度/CO2 + LTR-303 光照）
 *
 * SC7A20H 仍由本模块启动（供 C4004 安装角），但不进入环境快照上报。
 * 与雷达/区域配置无关，可独立于 radar_area 模块使用。
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 环境传感器快照 */
typedef struct {
    bool env_ready;
    float temperature;
    float humidity;
    int co2;
    bool lux_ready;
    float lux;
} env_snapshot_t;

/** @brief 启动 STCC4 / LTR-303 / SC7A20H 后台采样任务 */
void env_sensors_init(void);

/** @brief 读取最近一次环境传感器采样结果 */
esp_err_t env_sensors_get(env_snapshot_t *out);

#ifdef __cplusplus
}
#endif
