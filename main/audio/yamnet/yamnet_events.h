/**
 * @file yamnet_events.h
 * @brief 通用 AudioSet 类事件检测（阈值 + 时间窗投票）
 *
 * 新增事件只需改 yamnet_events.c 中的 s_event_table；分类器每帧喂入 softmax。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define YAMNET_EVENT_MAX_CLASSES 4
#define YAMNET_EVENT_VOTE_CAP    16
#define YAMNET_EVENT_MAX         8

/** 单个可检测事件的静态描述（表驱动） */
typedef struct {
    /** 应用/MCP 用稳定 id，如 "cry" */
    const char *id;
    /** 日志短名，如 "CRY" */
    const char *log_name;
    /** 参与得分求和的类别下标（-1 未用） */
    int16_t class_ids[YAMNET_EVENT_MAX_CLASSES];
    uint8_t class_count;
    /** 阈值，毫单位（220 → 0.220） */
    uint16_t score_thr_milli;
    uint8_t vote_window;
    uint8_t vote_need;
    uint32_t hold_ms;
    /**
     * 为 true 时：若 Speech 相对事件分过高则本帧不算命中
     * （利于哭声 vs 说话；玻璃碎等冲击声建议 false）
     */
    bool suppress_if_speech;
} yamnet_event_desc_t;

/** 投票通过且过冷却后触发的告警回调 */
typedef void (*yamnet_event_alert_cb_t)(const yamnet_event_desc_t *ev,
                                       float score,
                                       int votes,
                                       void *user_ctx);

/** 初始化运行时状态（投票窗、冷却时钟）。 */
void yamnet_events_init(void);

/**
 * 用一帧 softmax 更新所有已启用事件。
 * @param probs         概率向量
 * @param n_classes     有效类别数（通常 521）
 * @param speech_score  类别 0（Speech）概率
 * @param alert_cb      上升沿告警回调
 * @param user_ctx      传给 alert_cb
 */
void yamnet_events_on_probs(const float *probs,
                            int n_classes,
                            float speech_score,
                            yamnet_event_alert_cb_t alert_cb,
                            void *user_ctx);

/**
 * 按 event_id 对配置的类别概率求和；未知 id 返回 0。
 */
float yamnet_events_score(const char *event_id, const float *probs, int n_classes);

/** 内置事件描述个数。 */
int yamnet_events_count(void);

/** 按下标取事件描述；越界返回 NULL。 */
const yamnet_event_desc_t *yamnet_events_get(int index);

#ifdef __cplusplus
}
#endif
