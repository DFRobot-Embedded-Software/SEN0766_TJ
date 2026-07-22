/**
 * @file yamnet_classifier.h
 * @brief YAMNet INT8 声音事件分类（TFLite Micro）
 *
 * 仅依赖 yamnet_pcm_in / mel / events，不直接碰采集硬件或网络透传。
 */
#pragma once

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define YAMNET_TOP_K 5

/** 单次 Top-K 条目 */
typedef struct {
    int class_id;
    float score;
    char name[48];
} yamnet_topk_t;

/** 每次推理完成后的可选回调（Top-K 结果） */
typedef void (*yamnet_result_cb_t)(const yamnet_topk_t *top, int count, void *user_ctx);

/**
 * 启动分类器：挂载模型、加载 TFLite、启动 PCM 桥与推理任务。
 * 事件告警（cry/glass/…）由 yamnet_events 表驱动。
 */
esp_err_t yamnet_classifier_start(yamnet_result_cb_t cb, void *user_ctx);

/** 停止推理任务与 PCM 桥。 */
void yamnet_classifier_stop(void);

/**
 * 线程安全地拷贝最近一次 Top-K 快照。
 * @param out        输出数组（至少 YAMNET_TOP_K）
 * @param out_count  实际条目数
 */
esp_err_t yamnet_classifier_get_latest(yamnet_topk_t *out, int *out_count);

/**
 * 事件投票通过后的应用层回调。
 * @param event_id  如 "cry" / "glass"
 */
typedef void (*yamnet_event_app_cb_t)(const char *event_id, float score, int votes, void *user_ctx);

/** 注册事件告警回调（可选）。 */
void yamnet_classifier_set_event_cb(yamnet_event_app_cb_t cb, void *user_ctx);

#ifdef __cplusplus
}
#endif
