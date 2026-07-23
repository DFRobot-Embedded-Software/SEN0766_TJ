/**
 * @file ltr303_sensor.h
 * @brief LTR-303ALS-01 环境光传感器（I2C，与 STCC4 共用 I2C0）
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ltr303_sensor_start(void);
bool ltr303_sensor_is_ready(void);

/**
 * @brief 读取最近一次照度
 * @param lux 输出照度 (lx)
 */
esp_err_t ltr303_sensor_get(float *lux);

#ifdef __cplusplus
}
#endif
