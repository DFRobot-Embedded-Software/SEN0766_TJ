/**
 * @file ws2812_fx.c
 * @brief WS2812 基础灯效任务实现
 */
#include "ws2812_fx.h"
#include "ws2812_led.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#if CONFIG_WS2812_ENABLE

#ifndef CONFIG_WS2812_LED_COUNT
#define CONFIG_WS2812_LED_COUNT 7
#endif

#define LED_N ((int)CONFIG_WS2812_LED_COUNT)

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static volatile bool s_run;
static volatile bool s_busy;
static ws2812_fx_cfg_t s_cfg;
static uint32_t s_gen; /* 递增以打断旧循环 */

static void ensure_lock(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
}

ws2812_range_t ws2812_range_all(void)
{
    ws2812_range_t r = { .start = 0, .end = (uint8_t)(LED_N > 0 ? LED_N - 1 : 0) };
    return r;
}

static bool clamp_range(ws2812_range_t *r)
{
    if (r == NULL || LED_N <= 0) {
        return false;
    }
    if (r->start >= LED_N) {
        r->start = (uint8_t)(LED_N - 1);
    }
    if (r->end >= LED_N) {
        r->end = (uint8_t)(LED_N - 1);
    }
    if (r->start > r->end) {
        uint8_t t = r->start;
        r->start = r->end;
        r->end = t;
    }
    return true;
}

static void scale_rgb(uint8_t r, uint8_t g, uint8_t b, uint8_t bright, uint8_t *or, uint8_t *og, uint8_t *ob)
{
    *or = (uint8_t)((uint16_t)r * bright / 255);
    *og = (uint8_t)((uint16_t)g * bright / 255);
    *ob = (uint8_t)((uint16_t)b * bright / 255);
}

static void fill_range(const ws2812_range_t *rg, uint8_t r, uint8_t g, uint8_t b, uint8_t bright)
{
    uint8_t rr, gg, bb;
    scale_rgb(r, g, b, bright, &rr, &gg, &bb);
    for (int i = 0; i < LED_N; i++) {
        if (i >= rg->start && i <= rg->end) {
            (void)ws2812_led_set_pixel((uint32_t)i, rr, gg, bb);
        } else {
            (void)ws2812_led_set_pixel((uint32_t)i, 0, 0, 0);
        }
    }
    (void)ws2812_led_refresh();
}

static void clear_all(void)
{
    (void)ws2812_led_clear();
}

static bool still(uint32_t gen)
{
    return s_run && gen == s_gen;
}

static void delay_abortable(uint32_t gen, uint32_t ms)
{
    while (ms > 0 && still(gen)) {
        uint32_t slice = ms > 20 ? 20 : ms;
        vTaskDelay(pdMS_TO_TICKS(slice));
        ms -= slice;
    }
}

static void run_flow(uint32_t gen, ws2812_fx_flow_t p)
{
    if (!clamp_range(&p.range)) {
        return;
    }
    const int a = p.range.start;
    const int b = p.range.end;
    if (p.step_ms == 0) {
        p.step_ms = 40;
    }

    uint16_t loops_done = 0;
    int dir = (p.dir == WS2812_FLOW_REVERSE) ? -1 : 1;
    int pos = (dir > 0) ? a : b;

    while (still(gen)) {
        clear_all();
        uint8_t rr, gg, bb;
        scale_rgb(p.r, p.g, p.b, p.brightness ? p.brightness : 255, &rr, &gg, &bb);
        (void)ws2812_led_set_pixel((uint32_t)pos, rr, gg, bb);
        (void)ws2812_led_refresh();
        delay_abortable(gen, p.step_ms);
        if (!still(gen)) {
            break;
        }

        if (p.dir == WS2812_FLOW_PINGPONG) {
            int next = pos + dir;
            if (next > b || next < a) {
                dir = -dir;
                next = pos + dir;
                if (dir > 0) {
                    /* 刚从反向折回正向：完成一轮来回 */
                    loops_done++;
                }
            }
            pos = next;
        } else {
            pos += dir;
            if (pos > b) {
                pos = a;
                loops_done++;
            } else if (pos < a) {
                pos = b;
                loops_done++;
            }
        }

        if (p.loops != 0 && loops_done >= p.loops) {
            break;
        }
    }
}

static void run_breath(uint32_t gen, ws2812_fx_breath_t p)
{
    if (!clamp_range(&p.range)) {
        return;
    }
    if (p.period_ms < 40) {
        p.period_ms = 40;
    }
    if (p.bright_min > p.bright_max) {
        uint8_t t = p.bright_min;
        p.bright_min = p.bright_max;
        p.bright_max = t;
    }

    const int steps = 40;
    const uint32_t step_ms = p.period_ms / (uint32_t)steps;
    uint16_t loops_done = 0;

    while (still(gen)) {
        for (int i = 0; i <= steps && still(gen); i++) {
            float phase = (float)i / (float)steps; /* 0..1 */
            float wave = 0.5f - 0.5f * cosf(phase * 2.0f * (float)M_PI);
            uint8_t br = (uint8_t)(p.bright_min + (float)(p.bright_max - p.bright_min) * wave);
            fill_range(&p.range, p.r, p.g, p.b, br);
            delay_abortable(gen, step_ms);
        }
        loops_done++;
        if (p.loops != 0 && loops_done >= p.loops) {
            break;
        }
    }
}

static void run_blink(uint32_t gen, ws2812_fx_blink_t p)
{
    if (!clamp_range(&p.range)) {
        return;
    }
    if (p.interval_ms == 0) {
        p.interval_ms = 200;
    }
    uint16_t n = 0;
    while (still(gen)) {
        fill_range(&p.range, p.r, p.g, p.b, p.brightness ? p.brightness : 255);
        delay_abortable(gen, p.interval_ms);
        if (!still(gen)) {
            break;
        }
        clear_all();
        delay_abortable(gen, p.interval_ms);
        n++;
        if (p.count != 0 && n >= p.count) {
            break;
        }
    }
}

static void run_spread(uint32_t gen, ws2812_fx_spread_t p)
{
    if (!clamp_range(&p.range)) {
        return;
    }
    if (p.step_ms == 0) {
        p.step_ms = 80;
    }
    const int a = p.range.start;
    const int b = p.range.end;
    const int mid = (a + b) / 2;
    const int max_radius = (b - a + 1 + 1) / 2;
    uint16_t rounds = 0;

    while (still(gen)) {
        for (int rad = 0; rad <= max_radius && still(gen); rad++) {
            clear_all();
            uint8_t rr, gg, bb;
            scale_rgb(p.r, p.g, p.b, p.brightness ? p.brightness : 255, &rr, &gg, &bb);
            for (int i = a; i <= b; i++) {
                int d = i - mid;
                if (d < 0) {
                    d = -d;
                }
                if (d <= rad) {
                    (void)ws2812_led_set_pixel((uint32_t)i, rr, gg, bb);
                }
            }
            (void)ws2812_led_refresh();
            delay_abortable(gen, p.step_ms);
        }
        /* 略留全亮再灭，形成一轮扩散 */
        delay_abortable(gen, p.step_ms);
        clear_all();
        delay_abortable(gen, p.step_ms);
        rounds++;
        if (p.count != 0 && rounds >= p.count) {
            break;
        }
    }
}

static void run_static(uint32_t gen, ws2812_fx_static_t p)
{
    (void)gen;
    if (!clamp_range(&p.range)) {
        return;
    }
    if (!p.on) {
        clear_all();
        return;
    }
    fill_range(&p.range, p.r, p.g, p.b, p.brightness ? p.brightness : 255);
}

static void fx_task(void *arg)
{
    (void)arg;
    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!s_run) {
            continue;
        }

        ws2812_fx_cfg_t cfg;
        uint32_t gen;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        cfg = s_cfg;
        gen = s_gen;
        xSemaphoreGive(s_lock);

        s_busy = true;
        switch (cfg.mode) {
        case WS2812_FX_FLOW:
            run_flow(gen, cfg.u.flow);
            break;
        case WS2812_FX_BREATH:
            run_breath(gen, cfg.u.breath);
            break;
        case WS2812_FX_BLINK:
            run_blink(gen, cfg.u.blink);
            break;
        case WS2812_FX_SPREAD:
            run_spread(gen, cfg.u.spread);
            break;
        case WS2812_FX_STATIC:
            run_static(gen, cfg.u.stat);
            break;
        default:
            break;
        }
        if (gen == s_gen) {
            s_busy = false;
        }
    }
}

esp_err_t ws2812_fx_start(const ws2812_fx_cfg_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ensure_lock();

    if (s_task == NULL) {
        if (xTaskCreate(fx_task, "ws2812_fx", 3072, NULL, 3, &s_task) != pdPASS) {
            s_task = NULL;
            return ESP_ERR_NO_MEM;
        }
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cfg = *cfg;
    s_gen++;
    s_run = true;
    s_busy = true;
    xSemaphoreGive(s_lock);

    xTaskNotifyGive(s_task);
    ESP_LOGI(TAG, "fx start mode=%d", (int)cfg->mode);
    return ESP_OK;
}

void ws2812_fx_stop(void)
{
    ensure_lock();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_run = false;
    s_gen++;
    s_busy = false;
    xSemaphoreGive(s_lock);
    clear_all();
}

bool ws2812_fx_is_busy(void)
{
    return s_busy;
}

#else /* !CONFIG_WS2812_ENABLE */

ws2812_range_t ws2812_range_all(void)
{
    ws2812_range_t r = { 0, 0 };
    return r;
}

esp_err_t ws2812_fx_start(const ws2812_fx_cfg_t *cfg)
{
    (void)cfg;
    return ESP_ERR_NOT_SUPPORTED;
}

void ws2812_fx_stop(void) {}

bool ws2812_fx_is_busy(void)
{
    return false;
}

#endif /* CONFIG_WS2812_ENABLE */
