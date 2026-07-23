/**
 * @file stcc4_sensor.h
 * @brief DFRobot Gravity STCC4 (Sensirion) CO2 / 温湿度传感器
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>

/** @brief 初始化 I2C 并启动后台采样任务 */
esp_err_t stcc4_sensor_start(void);

/** @brief 传感器是否已成功初始化 */
bool stcc4_sensor_is_ready(void);

/**
 * @brief 读取最近一次采样结果
 * @return ESP_ERR_NOT_FOUND 传感器未就绪；ESP_ERR_INVALID_STATE 尚无有效数据
 */
esp_err_t stcc4_sensor_get(float *temperature, float *humidity, int *co2_ppm);
