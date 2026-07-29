/**
 * @file sc7a20h_sensor.h
 * @brief SC7A20H 三轴加速度计（I2C，LIS2DH 兼容寄存器，与 STCC4 共用 I2C0）
 *
 * 由重力分量判断是否垂直立地，并给出前后倾角；供 C4004 安装角配置使用（不上报 Web/MCP）。
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 垂直/前后倾姿态（由加速度重力分量估算，设备宜相对静止） */
typedef struct {
    bool upright;           /**< |前后倾| 在容差内视为垂直 */
    float tilt_deg;         /**< 有符号倾角：>0 向前，<0 向后 */
    float forward_deg;      /**< 向前倾角度（度），未向前则为 0 */
    float backward_deg;     /**< 向后倾角度（度），未向后则为 0 */
} sc7a20h_tilt_t;

esp_err_t sc7a20h_sensor_start(void);
bool sc7a20h_sensor_is_ready(void);

/**
 * @brief 读取最近一次加速度（单位 g）
 */
esp_err_t sc7a20h_sensor_get(float *ax, float *ay, float *az);

/**
 * @brief 读取最近一次垂直/倾角判定结果
 */
esp_err_t sc7a20h_sensor_get_tilt(sc7a20h_tilt_t *out);

#ifdef __cplusplus
}
#endif
