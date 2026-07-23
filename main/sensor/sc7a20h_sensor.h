/**
 * @file sc7a20h_sensor.h
 * @brief SC7A20H 三轴加速度计（I2C，LIS2DH 兼容寄存器，与 STCC4 共用 I2C0）
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t sc7a20h_sensor_start(void);
bool sc7a20h_sensor_is_ready(void);

/**
 * @brief 读取最近一次加速度（单位 g）
 */
esp_err_t sc7a20h_sensor_get(float *ax, float *ay, float *az);

#ifdef __cplusplus
}
#endif
