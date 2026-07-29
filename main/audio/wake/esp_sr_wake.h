/**
 * @file esp_sr_wake.h
 * @brief esp-sr 唤醒检测：订阅 audio_capture，检测到唤醒词后可启动透传
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 唤醒词命中回调（在 fetch 任务上下文，勿做重活） */
typedef void (*esp_sr_wake_cb_t)(void *user_ctx);

/**
 * 启动唤醒：加载 model 分区、创建 AFE、订阅采集、拉起 feed/fetch 任务。
 * 未开启 CONFIG_ESP_SR_WAKE_ENABLE 时返回 ESP_ERR_NOT_SUPPORTED。
 */
esp_err_t esp_sr_wake_start(void);

/** 停止唤醒任务并 release 采集。 */
void esp_sr_wake_stop(void);

/** 是否已启动。 */
bool esp_sr_wake_is_running(void);

/** 注册唤醒回调（可在 start 前后调用）。 */
void esp_sr_wake_set_callback(esp_sr_wake_cb_t cb, void *user_ctx);

#ifdef __cplusplus
}
#endif
