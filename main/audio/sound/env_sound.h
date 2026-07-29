/**
 * @file env_sound.h
 * @brief 从 ES7210 PCM 估算环境声级（近似 dB SPL）
 *
 * 订阅 audio_capture，用 RMS → dBFS + 参考偏置（可再叠加传感器校准 offset）。
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 启动：占用采集并注册 PCM sink。 */
esp_err_t env_sound_start(void);

/** 停止并释放采集引用。 */
void env_sound_stop(void);

/**
 * 读取当前环境声级（已含 Kconfig 参考偏置；调用方可再加校准 offset）。
 * @return true 已有有效读数
 */
bool env_sound_get_db(float *out_db);

/** 是否已启动且至少完成过一次 RMS 更新。 */
bool env_sound_ready(void);

#ifdef __cplusplus
}
#endif
