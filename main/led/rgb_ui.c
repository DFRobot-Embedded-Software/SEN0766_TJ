/**
 * @file rgb_ui.c
 * @brief 产品灯效：语音三态（识别/处理/播放）> 配网 > 日常
 */
#include "rgb_ui.h"
#include "ws2812_fx.h"
#include "ws2812_led.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

#include <string.h>

static const char *TAG = "rgb_ui";

#define M_CENTER (1u << 3)
#define M_AP     ((1u << 0) | (1u << 3) | (1u << 6))
#define M_LISTEN ((1u << 2) | (1u << 3) | (1u << 4))

#if CONFIG_WS2812_ENABLE

static SemaphoreHandle_t s_lock;
static rgb_net_t s_net;
static rgb_voice_t s_voice;
static rgb_net_t s_net_q;
static bool s_net_queued;
static bool s_selftest;

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

static bool build_fx(ws2812_fx_cfg_t *o)
{
    memset(o, 0, sizeof(*o));
    const ws2812_range_t all = ws2812_range_all();

    /* ---- 语音三态（最高优先）---- */
    if (s_voice == RGB_VOICE_LISTEN) {
        /* 识别：中间三灯蓝色呼吸 */
        o->mode = WS2812_FX_BREATH;
        o->u.breath = (ws2812_fx_breath_t){
            .r = 80, .g = 170, .b = 255,
            .bright_min = 30, .bright_max = 220,
            .period_ms = 1200, .range = all, .led_mask = M_LISTEN,
        };
        return false;
    }
    if (s_voice == RGB_VOICE_PROCESS) {
        /* 处理：蓝→青扩散循环，表示后台在算 */
        o->mode = WS2812_FX_SPREAD;
        o->u.spread = (ws2812_fx_spread_t){
            .r = 30, .g = 90, .b = 255, .r2 = 0, .g2 = 220, .b2 = 220,
            .use_grad = true, .brightness = 220, .count = 0, .step_ms = 70,
            .range = all,
        };
        return false;
    }
    if (s_voice == RGB_VOICE_PLAY) {
        /* 播放：暖色常亮 */
        o->mode = WS2812_FX_STATIC;
        o->u.stat = (ws2812_fx_static_t){
            .on = 1, .r = 255, .g = 180, .b = 90, .brightness = 160, .range = all,
        };
        return false;
    }

    /* ---- 配网 ---- */
    if (s_net == RGB_NET_SELFTEST) {
        o->mode = WS2812_FX_FLOW;
        o->u.flow = (ws2812_fx_flow_t){
            .dir = WS2812_FLOW_PINGPONG, .step_ms = 200, .loops = 1, .range = all,
            .r = 255, .g = 180, .b = 90, .brightness = 220,
        };
        return true;
    }
    if (s_net == RGB_NET_AP_WAIT) {
        o->mode = WS2812_FX_BREATH;
        o->u.breath = (ws2812_fx_breath_t){
            .r = 80, .g = 170, .b = 255, .bright_min = 20, .bright_max = 200,
            .period_ms = 1600, .pause_ms = 700, .range = all, .led_mask = M_AP,
        };
        return false;
    }
    if (s_net == RGB_NET_CONNECTING) {
        o->mode = WS2812_FX_GRAD_FLOW;
        o->u.grad = (ws2812_fx_grad_flow_t){
            .r = 30, .g = 90, .b = 255, .r2 = 255, .g2 = 255, .b2 = 255,
            .brightness = 200, .step_ms = 70, .range = all,
        };
        return false;
    }
    if (s_net == RGB_NET_OK) {
        o->mode = WS2812_FX_STATIC;
        o->u.stat = (ws2812_fx_static_t){
            .on = 1, .r = 255, .g = 255, .b = 255, .r2 = 30, .g2 = 90, .b2 = 255,
            .brightness = 220, .hold_ms = 1500, .range = all,
        };
        return true;
    }
    if (s_net == RGB_NET_FAIL) {
        o->mode = WS2812_FX_BLINK;
        o->u.blink = (ws2812_fx_blink_t){
            .r = 255, .g = 90, .b = 0, .brightness = 230, .count = 3,
            .interval_ms = 220, .range = all,
        };
        return true;
    }

    /* ---- 日常待机 ---- */
    o->mode = WS2812_FX_STATIC;
    o->u.stat = (ws2812_fx_static_t){
        .on = 1, .r = 80, .g = 170, .b = 255, .brightness = 40, .range = all,
    };
    (void)M_CENTER;
    return false;
}

static void render(void)
{
    ws2812_fx_cfg_t fx;
    bool finite = build_fx(&fx);
    if (s_net == RGB_NET_SELFTEST && s_voice == RGB_VOICE_NONE) {
        s_selftest = true;
    }
    if (finite) {
        (void)ws2812_fx_start_ex(&fx, on_fx_done, NULL);
    } else {
        (void)ws2812_fx_start(&fx);
    }
    ESP_LOGI(TAG, "voice=%d net=%d", (int)s_voice, (int)s_net);
}

static void on_fx_done(void *ctx)
{
    (void)ctx;
    lock();
    if (s_net == RGB_NET_SELFTEST) {
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
}

esp_err_t rgb_ui_init(void)
{
    s_net = RGB_NET_NONE;
    s_voice = RGB_VOICE_NONE;
    s_net_queued = false;
    s_selftest = false;
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
    /* 语音占用灯效时只记配网态，不打断语音灯 */
    if (s_voice == RGB_VOICE_NONE) {
        render();
    }
    unlock();
}

void rgb_ui_set_voice(rgb_voice_t voice)
{
    lock();
    if (s_voice == voice) {
        unlock();
        return;
    }
    s_voice = voice;
    render();
    unlock();
}

rgb_net_t rgb_ui_get_net(void)
{
    return s_net;
}

rgb_voice_t rgb_ui_get_voice(void)
{
    return s_voice;
}

#else

esp_err_t rgb_ui_init(void) { return ESP_OK; }
void rgb_ui_set_net(rgb_net_t n) { (void)n; }
void rgb_ui_set_voice(rgb_voice_t v) { (void)v; }
rgb_net_t rgb_ui_get_net(void) { return RGB_NET_NONE; }
rgb_voice_t rgb_ui_get_voice(void) { return RGB_VOICE_NONE; }

#endif
