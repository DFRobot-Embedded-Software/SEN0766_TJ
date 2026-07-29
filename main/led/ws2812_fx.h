/**
 * @file ws2812_fx.h
 * @brief WS2812 基础灯效（流水 / 呼吸 / 闪烁 / 扩散 / 静态 / 渐变流水）
 *
 * 新 start 会打断当前灯效；loops=0 表示无限循环（静态除外）。
 * led_mask!=0 时按位选择灯珠（bit0=灯珠1），忽略 range。
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
    WS2812_FX_FLOW = 0,    /**< 单点流水 */
    WS2812_FX_BREATH,      /**< 呼吸 */
    WS2812_FX_BLINK,       /**< 闪烁 */
    WS2812_FX_SPREAD,      /**< 扩散 */
    WS2812_FX_STATIC,      /**< 静态 */
    WS2812_FX_GRAD_FLOW,   /**< 全灯渐变循环流水（色1→色2） */
} ws2812_fx_mode_t;

typedef enum {
    WS2812_FLOW_FORWARD = 0,
    WS2812_FLOW_REVERSE,
    WS2812_FLOW_PINGPONG,
} ws2812_flow_dir_t;

typedef struct {
    ws2812_flow_dir_t dir;
    uint16_t step_ms;
    uint16_t loops;        /**< 0=无限；PINGPONG 时 1=正反各一圈 */
    ws2812_range_t range;
    uint8_t led_mask;      /**< 0=用 range */
    uint8_t r, g, b;
    uint8_t brightness;
} ws2812_fx_flow_t;

typedef struct {
    uint8_t r, g, b;
    uint8_t bright_min;
    uint8_t bright_max;
    uint16_t period_ms;
    uint16_t pause_ms;     /**< 间歇呼吸：周期之间熄灭时长 */
    uint16_t loops;
    ws2812_range_t range;
    uint8_t led_mask;
} ws2812_fx_breath_t;

typedef struct {
    uint8_t r, g, b;
    uint8_t brightness;
    uint16_t count;
    uint16_t interval_ms;
    ws2812_range_t range;
    uint8_t led_mask;
} ws2812_fx_blink_t;

typedef struct {
    uint8_t r, g, b;
    uint8_t r2, g2, b2;
    bool use_grad;
    uint8_t brightness;
    uint16_t count;
    uint16_t step_ms;
    ws2812_range_t range;
} ws2812_fx_spread_t;

typedef struct {
    bool on;
    uint8_t r, g, b;
    uint8_t r2, g2, b2;    /**< hold_ms>0 时从 (r,g,b) 渐变到 (r2,g2,b2) */
    uint8_t brightness;
    uint16_t hold_ms;      /**< >0 则保持/渐变后结束（触发 done） */
    ws2812_range_t range;
    uint8_t led_mask;
} ws2812_fx_static_t;

/** 全灯渐变循环：相位旋转，颜色在 (r,g,b)↔(r2,g2,b2) 间插值 */
typedef struct {
    uint8_t r, g, b;
    uint8_t r2, g2, b2;
    uint8_t brightness;
    uint16_t step_ms;
    uint16_t loops;        /**< 0=无限；一轮=LED_COUNT 步 */
    ws2812_range_t range;
} ws2812_fx_grad_flow_t;

typedef struct {
    ws2812_fx_mode_t mode;
    union {
        ws2812_fx_flow_t flow;
        ws2812_fx_breath_t breath;
        ws2812_fx_blink_t blink;
        ws2812_fx_spread_t spread;
        ws2812_fx_static_t stat;
        ws2812_fx_grad_flow_t grad;
    } u;
} ws2812_fx_cfg_t;

typedef void (*ws2812_fx_done_cb_t)(void *ctx);

ws2812_range_t ws2812_range_all(void);

esp_err_t ws2812_fx_start(const ws2812_fx_cfg_t *cfg);

/** 启动灯效；有限次动画结束后调用 done（被打断不调用）。 */
esp_err_t ws2812_fx_start_ex(const ws2812_fx_cfg_t *cfg, ws2812_fx_done_cb_t done, void *ctx);

void ws2812_fx_stop(void);

#ifdef __cplusplus
}
#endif
