/**
 * @file env_event_detector.h
 * @brief 环境阈值持续越限 → device_event_bus（category=env / sound_db_high）
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t env_event_detector_start(void);
void env_event_detector_stop(void);

#ifdef __cplusplus
}
#endif
