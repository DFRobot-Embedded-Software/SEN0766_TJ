/**
 * @file board_snapshot.h
 * @brief 组合 env_sensors + radar_area，供 Web /api/data 与 MCP env provider 共用
 */
#pragma once

#include "esp_err.h"
#include "radar_area.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 传感器快照，对应 GET /api/data 与 MCP sensors.get_snapshot */
typedef struct {
    bool env_ready;
    float temperature;
    float humidity;
    int co2;
    bool lux_ready;
    float lux;
    bool accel_ready;
    float ax;
    float ay;
    float az;
    uint8_t area_count;
    area_status_t areas[AREA_TAG_MAX];
} sensor_snapshot_t;

/** @brief 初始化 env_sensors 与 radar_area */
void board_snapshot_init(void);

/** @brief 读取合并后的整机快照 */
esp_err_t board_snapshot_get(sensor_snapshot_t *out);

#ifdef __cplusplus
}
#endif
