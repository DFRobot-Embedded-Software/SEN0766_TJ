/**
 * @file sensor_settings.h
 * @brief 环境异常判定阈值持久化（NVS JSON）；供 /api/sensors 与固件读取
 */
#pragma once

#include "esp_err.h"
#include "cJSON.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void sensor_settings_init(void);

/** @brief 返回 sensors 对象（调用方 cJSON_Delete）；失败返回带默认值的对象 */
cJSON *sensor_settings_to_json(void);

/** @brief 用前端 POST 的 sensors 对象覆盖并保存 */
esp_err_t sensor_settings_from_json(const cJSON *sensors);

/** @brief 声音传感器是否启用（默认 true） */
bool sensor_settings_sound_enabled(void);

/** @brief 声音校准偏置 dB（默认 0） */
float sensor_settings_sound_offset_db(void);

/**
 * @brief 毫米波安装高度（cm），供 C4004 下发
 * 优先读 sensors.mmwave.install_height_m；无效时回退 menuconfig 默认值
 */
uint16_t sensor_settings_install_height_cm(void);

#ifdef __cplusplus
}
#endif
