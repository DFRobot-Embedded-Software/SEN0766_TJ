/**
 * @file rgb_ui.c
 * @brief 产品灯效表 + 三层优先级导演（语音 > 配网 > 日常）
 */
#include "rgb_ui.h"
#include "ws2812_fx.h"
#include "ws2812_led.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

#include <string.h>

static const char *TAG = "rgb_ui";
#define LISTEN_TIMEOUT_MS 10000

#define M_CENTER  (1u << 3)
#define M_AP      ((1u << 0) | (1u << 3) | (1u << 6))
#define M_LISTEN  ((1u << 2) | (1u << 3) | (1u << 4))
#define M_CMDFAIL ((1u << 1) | (1u << 2) | (1u << 3) | (1u << 4) | (1u << 5))

#if CONFIG_WS2812_ENABLE

static SemaphoreHandle_t s_lock;
static rgb_net_t s_net;
static rgb_voice_t s_voice;
static rgb_net_t s_net_q;
static bool s_net_queued;
static bool s_selftest;
static bool s_tts;
static uint8_t s_tts_rgb[3];
static esp_timer_handle_t s_listen_tmr;

static void render(void);
static void on_fx_done(void *ctx);

static void lock(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_lock);
}

static void listen_tmr_stop(void)
{
    if (s_listen_tmr) {
        esp_timer_stop(s_listen_tmr);
    }
}

static void on_listen_timeout(void *arg)
{
    (void)arg;
    rgb_ui_set_voice(RGB_VOICE_TIMEOUT);
}

static void listen_tmr_arm(void)
{
    if (!s_listen_tmr) {
        const esp_timer_create_args_t a = {.callback = on_listen_timeout, .name = "rgb_listen"};
        if (esp_timer_create(&a, &s_listen_tmr) != ESP_OK) {
            return;
        }
    }
    esp_timer_stop(s_listen_tmr);
    esp_timer_start_once(s_listen_tmr, (uint64_t)LISTEN_TIMEOUT_MS * 1000ULL);
}

static bool build_fx(ws2812_fx_cfg_t *o)
{
    memset(o, 0, sizeof(*o));
    const ws2812_range_t all = ws2812_range_all();

    if (s_voice == RGB_VOICE_WAKE_OK) {
        o->mode = WS2812_FX_STATIC;
        o->u.stat = (ws2812_fx_static_t){.on = 1, .r = 255, .g = 255, .b = 255, .brightness = 255,
                                         .hold_ms = 200, .range = all, .led_mask = M_CENTER};
        return true;
    }
    if (s_voice == RGB_VOICE_LISTEN) {
        o->mode = WS2812_FX_BREATH;
        o->u.breath = (ws2812_fx_breath_t){.r = 80, .g = 170, .b = 255, .bright_min = 30, .bright_max = 220,
                                           .period_ms = 1200, .range = all, .led_mask = M_LISTEN};
        return false;
    }
    if (s_voice == RGB_VOICE_PARSE) {
        o->mode = WS2812_FX_SPREAD;
        o->u.spread = (ws2812_fx_spread_t){.r = 30, .g = 90, .b = 255, .r2 = 0, .g2 = 220, .b2 = 220,
                                           .use_grad = true, .brightness = 220, .count = 2, .step_ms = 70,
                                           .range = all};
        return true;
    }
    if (s_voice == RGB_VOICE_CMD_OK) {
        o->mode = WS2812_FX_FLOW;
        o->u.flow = (ws2812_fx_flow_t){.dir = WS2812_FLOW_FORWARD, .step_ms = 80, .loops = 1, .range = all,
                                       .r = 40, .g = 220, .b = 80, .brightness = 230};
        return true;
    }
    if (s_voice == RGB_VOICE_CMD_FAIL) {
        o->mode = WS2812_FX_BLINK;
        o->u.blink = (ws2812_fx_blink_t){.r = 255, .g = 90, .b = 0, .brightness = 230, .count = 2,
                                         .interval_ms = 200, .range = all, .led_mask = M_CMDFAIL};
        return true;
    }
    if (s_voice == RGB_VOICE_TTS) {
        o->mode = WS2812_FX_STATIC;
        o->u.stat = (ws2812_fx_static_t){.on = 1, .r = 255, .g = 180, .b = 90, .brightness = 160, .range = all};
        return false;
    }
    if (s_voice == RGB_VOICE_TIMEOUT) {
        o->mode = WS2812_FX_BLINK;
        o->u.blink = (ws2812_fx_blink_t){.r = 255, .g = 140, .b = 40, .brightness = 220, .count = 2,
                                         .interval_ms = 400, .range = all, .led_mask = M_CENTER};
        return true;
    }

    if (s_net == RGB_NET_SELFTEST) {
        o->mode = WS2812_FX_FLOW;
        o->u.flow = (ws2812_fx_flow_t){.dir = WS2812_FLOW_PINGPONG, .step_ms = 200, .loops = 1, .range = all,
                                       .r = 255, .g = 180, .b = 90, .brightness = 220};
        return true;
    }
    if (s_net == RGB_NET_AP_WAIT) {
        o->mode = WS2812_FX_BREATH;
        o->u.breath = (ws2812_fx_breath_t){.r = 80, .g = 170, .b = 255, .bright_min = 20, .bright_max = 200,
                                           .period_ms = 1600, .pause_ms = 700, .range = all, .led_mask = M_AP};
        return false;
    }
    if (s_net == RGB_NET_CONNECTING) {
        o->mode = WS2812_FX_GRAD_FLOW;
        o->u.grad = (ws2812_fx_grad_flow_t){.r = 30, .g = 90, .b = 255, .r2 = 255, .g2 = 255, .b2 = 255,
                                            .brightness = 200, .step_ms = 70, .range = all};
        return false;
    }
    if (s_net == RGB_NET_OK) {
        o->mode = WS2812_FX_STATIC;
        o->u.stat = (ws2812_fx_static_t){.on = 1, .r = 255, .g = 255, .b = 255, .r2 = 30, .g2 = 90, .b2 = 255,
                                         .brightness = 220, .hold_ms = 1500, .range = all};
        return true;
    }
    if (s_net == RGB_NET_FAIL) {
        o->mode = WS2812_FX_BLINK;
        o->u.blink = (ws2812_fx_blink_t){.r = 255, .g = 90, .b = 0, .brightness = 230, .count = 3,
                                         .interval_ms = 220, .range = all};
        return true;
    }

    o->mode = WS2812_FX_STATIC;
    o->u.stat = (ws2812_fx_static_t){.on = 1, .r = 80, .g = 170, .b = 255, .brightness = 40, .range = all};
    return false;
}

static void render(void)
{
    ws2812_fx_cfg_t fx;
    bool finite = build_fx(&fx);
    s_tts = (s_voice == RGB_VOICE_TTS);
    if (s_tts) {
        s_tts_rgb[0] = fx.u.stat.r;
        s_tts_rgb[1] = fx.u.stat.g;
        s_tts_rgb[2] = fx.u.stat.b;
    }
    if (s_net == RGB_NET_SELFTEST && s_voice == RGB_VOICE_NONE) {
        s_selftest = true;
    }
    if (finite) {
        (void)ws2812_fx_start_ex(&fx, on_fx_done, NULL);
    } else {
        (void)ws2812_fx_start(&fx);
    }
    ESP_LOGI(TAG, "v=%d n=%d", (int)s_voice, (int)s_net);
}

static void on_fx_done(void *ctx)
{
    (void)ctx;
    bool arm = false;
    lock();
    if (s_voice == RGB_VOICE_WAKE_OK) {
        s_voice = RGB_VOICE_LISTEN;
        arm = true;
        render();
    } else if (s_voice == RGB_VOICE_PARSE || s_voice == RGB_VOICE_CMD_OK || s_voice == RGB_VOICE_CMD_FAIL ||
               s_voice == RGB_VOICE_TIMEOUT) {
        s_voice = RGB_VOICE_NONE;
        listen_tmr_stop();
        render();
    } else if (s_net == RGB_NET_SELFTEST) {
        s_selftest = false;
        s_net = s_net_queued ? s_net_q : RGB_NET_NONE;
        s_net_queued = false;
        render();
    } else if (s_net == RGB_NET_OK) {
        s_net = RGB_NET_NONE;
        render();
    } else if (s_net == RGB_NET_FAIL) {
        s_net = RGB_NET_AP_WAIT;
        render();
    }
    unlock();
    if (arm) {
        listen_tmr_arm();
    }
}

esp_err_t rgb_ui_init(void)
{
    s_net = RGB_NET_NONE;
    s_voice = RGB_VOICE_NONE;
    s_net_queued = false;
    s_selftest = false;
    s_tts = false;
    return ESP_OK;
}

void rgb_ui_set_net(rgb_net_t net)
{
    lock();
    if (s_selftest && net != RGB_NET_SELFTEST) {
        s_net_q = net;
        s_net_queued = true;
        unlock();
        return;
    }
    if (s_net == RGB_NET_FAIL && net == RGB_NET_AP_WAIT) {
        unlock();
        return;
    }
    s_net = net;
    render();
    unlock();
}

void rgb_ui_set_voice(rgb_voice_t voice)
{
    bool arm = false;
    lock();
    if (voice != RGB_VOICE_LISTEN && voice != RGB_VOICE_TIMEOUT) {
        listen_tmr_stop();
    }
    arm = (voice == RGB_VOICE_LISTEN);
    s_voice = voice;
    render();
    unlock();
    if (arm) {
        listen_tmr_arm();
    }
}

void rgb_ui_tts_level(uint8_t br)
{
    if (!s_tts) {
        return;
    }
    uint8_t r = (uint8_t)((uint16_t)s_tts_rgb[0] * br / 255);
    uint8_t g = (uint8_t)((uint16_t)s_tts_rgb[1] * br / 255);
    uint8_t b = (uint8_t)((uint16_t)s_tts_rgb[2] * br / 255);
    for (int i = 0; i < CONFIG_WS2812_LED_COUNT; i++) {
        (void)ws2812_led_set_pixel((uint32_t)i, r, g, b);
    }
    (void)ws2812_led_refresh();
}

rgb_net_t rgb_ui_get_net(void)
{
    return s_net;
}

#else

esp_err_t rgb_ui_init(void) { return ESP_OK; }
void rgb_ui_set_net(rgb_net_t n) { (void)n; }
void rgb_ui_set_voice(rgb_voice_t v) { (void)v; }
void rgb_ui_tts_level(uint8_t b) { (void)b; }
rgb_net_t rgb_ui_get_net(void) { return RGB_NET_NONE; }

#endif
