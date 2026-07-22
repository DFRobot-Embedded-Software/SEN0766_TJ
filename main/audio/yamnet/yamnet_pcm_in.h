/**
 * @file yamnet_pcm_in.h
 * @brief YAMNet 专用 PCM 输入桥：私有 ring + 订阅 audio_capture
 *
 * 不打开 ES8388/I2S；与透传、esp-sr 互不依赖。
 */
#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 启动：创建私有 ring、acquire 采集并注册 sink。
 */
esp_err_t yamnet_pcm_in_start(void);

/**
 * 停止：注销 sink、release 采集、销毁 ring。
 */
void yamnet_pcm_in_stop(void);

/**
 * 非阻塞读取私有 ring 中的 PCM。
 * @param out          输出缓冲
 * @param max_samples  最多读取样本数
 * @return 实际写入的样本数（可能为 0）
 */
size_t yamnet_pcm_in_read(int16_t *out, size_t max_samples);

/**
 * 私有 ring 中待读字节数（无数据时为 0）。
 */
size_t yamnet_pcm_in_bytes_pending(void);

/**
 * 丢弃最多 max_bytes 旧数据（推理过慢时追赶实时）。
 */
void yamnet_pcm_in_discard_bytes(size_t max_bytes);

#ifdef __cplusplus
}
#endif
