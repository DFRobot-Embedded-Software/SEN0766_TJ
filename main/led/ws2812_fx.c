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
static ws2812_fx_cfg_t s_cfg;
static uint32_t s_gen;
static ws2812_fx_done_cb_t s_done_cb;
static void *s_done_ctx;

static const char *TAG = "ws2812_fx";

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

static bool led_selected(int i, uint8_t mask, const ws2812_range_t *rg)
{
    if (mask != 0) {
        return (mask & (1u << i)) != 0;
    }
    return i >= rg->start && i <= rg->end;
}

static void fill_leds(uint8_t mask, const ws2812_range_t *rg, uint8_t r, uint8_t g, uint8_t b, uint8_t bright)
{
    uint8_t rr, gg, bb;
    scale_rgb(r, g, b, bright, &rr, &gg, &bb);
    for (int i = 0; i < LED_N; i++) {
        if (led_selected(i, mask, rg)) {
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
        if (led_selected(pos, p.led_mask, &p.range)) {
            (void)ws2812_led_set_pixel((uint32_t)pos, rr, gg, bb);
        }
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

static void run_grad_flow(uint32_t gen, ws2812_fx_grad_flow_t p)
{
    if (!clamp_range(&p.range)) {
        return;
    }
    if (p.step_ms == 0) {
        p.step_ms = 60;
    }
    const int n = p.range.end - p.range.start + 1;
    if (n <= 0) {
        return;
    }
    const uint8_t bright = p.brightness ? p.brightness : 255;
    uint16_t loops_done = 0;
    int phase = 0;

    while (still(gen)) {
        for (int i = p.range.start; i <= p.range.end; i++) {
            int idx = i - p.range.start;
            float t = (float)((idx + phase) % n) / (float)(n > 1 ? n - 1 : 1);
            if (t > 1.0f) {
                t = 1.0f;
            }
            uint8_t r = (uint8_t)((float)p.r + ((float)p.r2 - (float)p.r) * t);
            uint8_t g = (uint8_t)((float)p.g + ((float)p.g2 - (float)p.g) * t);
            uint8_t b = (uint8_t)((float)p.b + ((float)p.b2 - (float)p.b) * t);
            uint8_t rr, gg, bb;
            scale_rgb(r, g, b, bright, &rr, &gg, &bb);
            (void)ws2812_led_set_pixel((uint32_t)i, rr, gg, bb);
        }
        (void)ws2812_led_refresh();
        delay_abortable(gen, p.step_ms);
        if (!still(gen)) {
            break;
        }
        phase = (phase + 1) % n;
        if (phase == 0) {
            loops_done++;
            if (p.loops != 0 && loops_done >= p.loops) {
                break;
            }
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
            float phase = (float)i / (float)steps;
            float wave = 0.5f - 0.5f * cosf(phase * 2.0f * (float)M_PI);
            uint8_t br = (uint8_t)(p.bright_min + (float)(p.bright_max - p.bright_min) * wave);
            fill_leds(p.led_mask, &p.range, p.r, p.g, p.b, br);
            delay_abortable(gen, step_ms);
        }
        if (p.pause_ms > 0 && still(gen)) {
            clear_all();
            delay_abortable(gen, p.pause_ms);
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
        fill_leds(p.led_mask, &p.range, p.r, p.g, p.b, p.brightness ? p.brightness : 255);
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
            float gt = (max_radius > 0) ? ((float)rad / (float)max_radius) : 0.0f;
            uint8_t cr = p.r, cg = p.g, cb = p.b;
            if (p.use_grad) {
                cr = (uint8_t)((float)p.r + ((float)p.r2 - (float)p.r) * gt);
                cg = (uint8_t)((float)p.g + ((float)p.g2 - (float)p.g) * gt);
                cb = (uint8_t)((float)p.b + ((float)p.b2 - (float)p.b) * gt);
            }
            uint8_t rr, gg, bb;
            scale_rgb(cr, cg, cb, p.brightness ? p.brightness : 255, &rr, &gg, &bb);
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
    if (!clamp_range(&p.range)) {
        return;
    }
    if (!p.on) {
        clear_all();
        return;
    }

    const uint8_t bright = p.brightness ? p.brightness : 255;
    if (p.hold_ms == 0) {
        fill_leds(p.led_mask, &p.range, p.r, p.g, p.b, bright);
        return;
    }

    /* 白→蓝等：在 hold_ms 内线性渐变 */
    const int steps = 30;
    const uint32_t step_ms = p.hold_ms / (uint32_t)steps;
    for (int i = 0; i <= steps && still(gen); i++) {
        float t = (float)i / (float)steps;
        uint8_t r = (uint8_t)((float)p.r + ((float)p.r2 - (float)p.r) * t);
        uint8_t g = (uint8_t)((float)p.g + ((float)p.g2 - (float)p.g) * t);
        uint8_t b = (uint8_t)((float)p.b + ((float)p.b2 - (float)p.b) * t);
        fill_leds(p.led_mask, &p.range, r, g, b, bright);
        delay_abortable(gen, step_ms > 0 ? step_ms : 1);
    }
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
        ws2812_fx_done_cb_t done;
        void *done_ctx;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        cfg = s_cfg;
        gen = s_gen;
        done = s_done_cb;
        done_ctx = s_done_ctx;
        xSemaphoreGive(s_lock);

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
        case WS2812_FX_GRAD_FLOW:
            run_grad_flow(gen, cfg.u.grad);
            break;
        default:
            break;
        }

        /* 有限动画正常结束；被打断时 gen 已变，不回调 */
        if (gen == s_gen && done != NULL) {
            done(done_ctx);
        }
    }
}

esp_err_t ws2812_fx_start(const ws2812_fx_cfg_t *cfg)
{
    return ws2812_fx_start_ex(cfg, NULL, NULL);
}

esp_err_t ws2812_fx_start_ex(const ws2812_fx_cfg_t *cfg, ws2812_fx_done_cb_t done, void *ctx)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ensure_lock();

    if (s_task == NULL) {
        if (xTaskCreate(fx_task, "ws2812_fx", 4096, NULL, 3, &s_task) != pdPASS) {
            s_task = NULL;
            return ESP_ERR_NO_MEM;
        }
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cfg = *cfg;
    s_done_cb = done;
    s_done_ctx = ctx;
    s_gen++;
    s_run = true;
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
    s_done_cb = NULL;
    s_done_ctx = NULL;
    xSemaphoreGive(s_lock);
    clear_all();
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

esp_err_t ws2812_fx_start_ex(const ws2812_fx_cfg_t *cfg, ws2812_fx_done_cb_t done, void *ctx)
{
    (void)cfg;
    (void)done;
    (void)ctx;
    return ESP_ERR_NOT_SUPPORTED;
}

void ws2812_fx_stop(void) {}


#endif /* CONFIG_WS2812_ENABLE */
