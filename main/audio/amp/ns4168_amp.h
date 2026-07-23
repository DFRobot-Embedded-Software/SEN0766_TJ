/**
 * @file ns4168_amp.h
 * @brief NS4168 I2S Class-D 功放（独立 TX，与 ES7210 录音分离）
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 初始化 I2S TX。未开启 CONFIG_NS4168_ENABLE 时为空操作。
 */
esp_err_t ns4168_amp_init(void);

/** 关闭 I2S 并释放资源。 */
void ns4168_amp_deinit(void);

/** 硬件是否已就绪。 */
bool ns4168_amp_is_ready(void);

/**
 * 写入单声道 int16 PCM（采样率见 menuconfig NS4168_SAMPLE_RATE）。
 * @param pcm       样本缓冲
 * @param n_samples 样本数
 * @return ESP_OK 或写入错误
 */
esp_err_t ns4168_amp_write(const int16_t *pcm, size_t n_samples);

/**
 * 使能/静音输出：关闭时停 I2S TX；再次 enable 会重新打开 channel。
 */
esp_err_t ns4168_amp_set_enabled(bool enabled);

#ifdef __cplusplus
}
#endif
