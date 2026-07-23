/**
 * @file ws2812_fx.h
 * @brief WS2812 基础灯效（流水 / 呼吸 / 闪烁 / 扩散 / 静态），默认 7 灯
 *
 * 新 start 会打断当前灯效；loops=0 表示无限循环（静态除外）。
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 灯珠闭区间范围 [start, end]，需 start <= end < LED_COUNT */
typedef struct {
    uint8_t start;
    uint8_t end;
} ws2812_range_t;

typedef enum {
    WS2812_FX_FLOW = 0,   /**< 流水 */
    WS2812_FX_BREATH,     /**< 呼吸 */
    WS2812_FX_BLINK,      /**< 闪烁 */
    WS2812_FX_SPREAD,     /**< 扩散 */
    WS2812_FX_STATIC,     /**< 静态 */
} ws2812_fx_mode_t;

/** 流水方向 */
typedef enum {
    WS2812_FLOW_FORWARD = 0, /**< 正向：start → end */
    WS2812_FLOW_REVERSE,     /**< 反向：end → start */
    WS2812_FLOW_PINGPONG,    /**< 双向：来回 */
} ws2812_flow_dir_t;

/** 流水参数 */
typedef struct {
    ws2812_flow_dir_t dir;
    uint16_t step_ms;      /**< 每步间隔，越小越快 */
    uint16_t loops;        /**< 循环次数，0=无限 */
    ws2812_range_t range;
    uint8_t r, g, b;       /**< 亮点颜色 */
    uint8_t brightness;    /**< 0–255 */
} ws2812_fx_flow_t;

/** 呼吸参数 */
typedef struct {
    uint8_t r, g, b;
    uint8_t bright_min;    /**< 亮度下限 0–255 */
    uint8_t bright_max;    /**< 亮度上限 0–255 */
    uint16_t period_ms;    /**< 一个呼吸周期（暗→亮→暗） */
    uint16_t loops;        /**< 0=无限 */
    ws2812_range_t range;  /**< 参与灯珠 */
} ws2812_fx_breath_t;

/** 闪烁参数 */
typedef struct {
    uint8_t r, g, b;
    uint8_t brightness;
    uint16_t count;        /**< 闪烁次数，0=无限 */
    uint16_t interval_ms;  /**< 亮/灭各持续时长 */
    ws2812_range_t range;
} ws2812_fx_blink_t;

/** 扩散参数：从 range 中心向两端逐步点亮 */
typedef struct {
    uint8_t r, g, b;
    uint8_t brightness;
    uint16_t count;        /**< 扩散轮次，0=无限 */
    uint16_t step_ms;      /**< 每扩一层间隔 */
    ws2812_range_t range;
} ws2812_fx_spread_t;

/** 静态参数 */
typedef struct {
    bool on;               /**< true=常亮，false=常灭 */
    uint8_t r, g, b;
    uint8_t brightness;
    ws2812_range_t range;
} ws2812_fx_static_t;

typedef struct {
    ws2812_fx_mode_t mode;
    union {
        ws2812_fx_flow_t flow;
        ws2812_fx_breath_t breath;
        ws2812_fx_blink_t blink;
        ws2812_fx_spread_t spread;
        ws2812_fx_static_t stat;
    } u;
} ws2812_fx_cfg_t;

/** 便捷：整条灯带范围（0 .. LED_COUNT-1） */
ws2812_range_t ws2812_range_all(void);

/**
 * 启动灯效（打断当前效果）。需先 ws2812_led_init()。
 */
esp_err_t ws2812_fx_start(const ws2812_fx_cfg_t *cfg);

/** 停止灯效并熄灭全部像素。 */
void ws2812_fx_stop(void);

/** 是否有动画任务在跑（静态完成后不算 busy）。 */
bool ws2812_fx_is_busy(void);

#ifdef __cplusplus
}
#endif
