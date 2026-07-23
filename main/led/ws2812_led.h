/**
 * @file ws2812_led.h
 * @brief WS2812 / SK6812 彩灯控制（默认 GPIO21）
 */
#pragma once

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 初始化 RMT 驱动。未开启 CONFIG_WS2812_ENABLE 时为空操作。
 * 灯效请用 ws2812_fx_start()。
 */
esp_err_t ws2812_led_init(void);

/** 释放灯带资源。 */
void ws2812_led_deinit(void);

/**
 * 设置全部像素为同一 RGB（0–255），并立即刷新。
 */
esp_err_t ws2812_led_set_rgb(uint8_t r, uint8_t g, uint8_t b);

/**
 * 设置第 index 颗灯的 RGB（0 .. LED_COUNT-1）。
 */
esp_err_t ws2812_led_set_pixel(uint32_t index, uint8_t r, uint8_t g, uint8_t b);

/** 刷新显示（改像素后若未走 set_rgb 需手动调用）。 */
esp_err_t ws2812_led_refresh(void);

/** 全部熄灭。 */
esp_err_t ws2812_led_clear(void);

#ifdef __cplusplus
}
#endif
