/**
 * @file audio_ws_client.c
 * @brief WebSocket 客户端音频会话（上行 mic / 下行 amp）
 *
 * 会话：connect → uplink → 等下行 → play_stop 排空 → 可选续听窗口 → 再 uplink 或 dialog_idle。
 * 关键：事件回调里禁止阻塞（尤其禁止 ns4168_amp_write）；下行 PCM 入 ring，由 worker 播放。
 */
#include "audio_ws_client.h"
#include "audio_capture.h"
#include "ns4168_amp.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "rgb_ui.h"
#include "sdkconfig.h"

#if CONFIG_AUDIO_WS_ENABLE
#include "esp_websocket_client.h"
#if CONFIG_MCP_GW_CLIENT_ENABLE
#include "mcp_gw_client.h"
#endif
#endif

static const char *TAG = "audio_ws";

#if CONFIG_AUDIO_WS_ENABLE

#define WS_TX_RB_BYTES (8000 * sizeof(int16_t))
#define WS_RX_RB_BYTES (16000 * sizeof(int16_t) * 12) /* ~12s 下行 PSRAM */
#define WS_SEND_CHUNK (320 * sizeof(int16_t))         /* 20ms @ 16kHz */
#define WS_PLAY_CHUNK (320 * sizeof(int16_t))
#define WS_CMD_QUEUE_LEN 8
#define WS_SEND_TIMEOUT_MS 1500 /* 过短易 poll_write(0)→组件直接 abort 整条连接 */
#define WS_SEND_FAIL_LIMIT 1
#define WS_RECONNECT_COOLDOWN_MS 3000
#define WS_TASK_STACK 5120
#define WS_BUFFER_SIZE 8192
#define WS_WORKER_STACK 6144
#define WS_TASK_PRIO 7   /* 高于播放 worker，避免长音频时 TCP 收包饿死 → Send-Q 卡死 */
#define WS_WORKER_PRIO 4
/* 播前攒满再开功放；过早 enable + auto_clear 会先吐静音，开头听起来像卡顿 */
#define WS_PREBUFFER_BYTES (16000 * sizeof(int16_t)) /* ~1.0s */

typedef enum {
    WS_CMD_SESSION_START = 1,
    WS_CMD_SESSION_STOP,
    WS_CMD_EOS,
    WS_CMD_CLOSE,
    WS_CMD_BEGIN_UPLINK,
    WS_CMD_PLAY_START, /* 停上行(若仍在) + 开功放 */
    WS_CMD_LISTEN_STOP, /* 网关 ASR 出句，立刻停上行 */
    WS_CMD_DIALOG_IDLE, /* 续听超时 / FOLLOWUP=0：通知网关清上下文 */
    WS_CMD_FOLLOWUP_ABORT, /* 上行已开但无语音：停上行 + dialog_idle，不 await */
} ws_cmd_t;

typedef struct {
    ws_cmd_t cmd;
    char reason[12];
} ws_cmd_msg_t;

static RingbufHandle_t s_tx_rb;
static RingbufHandle_t s_rx_rb;
static QueueHandle_t s_cmd_q;
static TaskHandle_t s_worker;
static SemaphoreHandle_t s_lock;
static esp_websocket_client_handle_t s_client;
static int s_sink_id = -1;
static bool s_inited;
static volatile bool s_worker_run;
static volatile bool s_connected;
static volatile bool s_uplink;
static volatile bool s_tx_inhibit; /* listen_stop/play 后禁止任何 send_*，否则会 abort 连接 */
static volatile bool s_playing;
static volatile bool s_start_pending;
static volatile bool s_await_downlink;
static volatile bool s_drain_then_close;
static volatile bool s_ws_dead; /* 连接已废，下一轮 session_start 再 destroy+重建 */
static volatile bool s_play_primed; /* 已攒够预缓冲，开始真正写功放 */
static volatile bool s_followup_armed; /* 播完后续听窗口（不算 busy） */
static volatile bool s_skip_followup;  /* 网关 dialog_end：播完后不续听 */
static volatile bool s_uplink_expect_speech; /* 本轮上行需先听到语音，否则短超时 abort */
static volatile bool s_speech_preheard; /* begin_uplink 前已确认有语音（消异步竞态） */
static int64_t s_deadline_us;
static int64_t s_downlink_deadline_us;
static int64_t s_followup_deadline_us;
static int64_t s_nospeech_deadline_us;
static int64_t s_tx_origin_us;
static size_t s_tx_samples_sent;
static size_t s_rx_bytes_total;
static int s_send_fail_streak;
static int64_t s_cooldown_until_us;
static char s_auth_headers[192];

static void voice_listen(void)
{
    rgb_ui_set_voice(RGB_VOICE_LISTEN);
}

static void voice_process(void)
{
    rgb_ui_set_voice(RGB_VOICE_PROCESS);
}

static void voice_play(void)
{
    rgb_ui_set_voice(RGB_VOICE_PLAY);
}

static void voice_idle(void)
{
    rgb_ui_set_voice(RGB_VOICE_NONE);
}

static void post_cmd(ws_cmd_t cmd, const char *reason);

static void drain_rb(RingbufHandle_t rb)
{
    if (rb == NULL) {
        return;
    }
    for (;;) {
        size_t item_size = 0;
        void *item = xRingbufferReceiveUpTo(rb, &item_size, 0, 2048);
        if (item == NULL) {
            break;
        }
        vRingbufferReturnItem(rb, item);
    }
}

static void make_room(RingbufHandle_t rb, size_t capacity, size_t need_bytes)
{
    if (rb == NULL) {
        return;
    }
    for (;;) {
        UBaseType_t waiting = 0;
        vRingbufferGetInfo(rb, NULL, NULL, NULL, NULL, &waiting);
        if ((size_t)waiting + need_bytes <= capacity) {
            break;
        }
        size_t item_size = 0;
        void *item = xRingbufferReceiveUpTo(rb, &item_size, 0, need_bytes);
        if (item == NULL) {
            break;
        }
        vRingbufferReturnItem(rb, item);
    }
}

static void send_text_op(const char *json)
{
    if (s_client == NULL || !s_connected || json == NULL) {
        return;
    }
    int n = (int)strlen(json);
    int ret = esp_websocket_client_send_text(s_client, json, n, pdMS_TO_TICKS(1000));
    if (ret < 0) {
        ESP_LOGW(TAG, "send_text failed (%d): %s", ret, json);
    } else {
        ESP_LOGI(TAG, ">> %s", json);
    }
}

static void send_text_op_if_tx_ok(const char *json)
{
    if (s_tx_inhibit) {
        return;
    }
    send_text_op(json);
}

static void stop_uplink_locked(const char *reason)
{
    if (!s_uplink) {
        return;
    }
    s_uplink = false;
    drain_rb(s_tx_rb);
    if (s_tx_inhibit) {
        ESP_LOGI(TAG, "uplink stopped (%s), tx inhibited — skip uplink_stop",
                 reason ? reason : "manual");
        return;
    }
    char msg[96];
    snprintf(msg, sizeof(msg),
             "{\"op\":\"uplink_stop\",\"reason\":\"%s\"}",
             (reason && reason[0]) ? reason : "manual");
    send_text_op_if_tx_ok(msg);
    vTaskDelay(pdMS_TO_TICKS(30));
    ESP_LOGI(TAG, "uplink stopped (%s)", reason ? reason : "manual");
}

static void destroy_client(void)
{
    if (s_client != NULL) {
        /* 勿先 close 再 destroy：对端常已断开，close 易卡住；destroy 内部会 stop 任务 */
        esp_err_t err = esp_websocket_client_stop(s_client);
        if (err != ESP_OK) {
            ESP_LOGD(TAG, "ws stop: %s", esp_err_to_name(err));
        }
        err = esp_websocket_client_destroy(s_client);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "ws destroy: %s", esp_err_to_name(err));
        }
        s_client = NULL;
        /* 等任务/套接字释放，过短易二次 connect delayed abort */
        vTaskDelay(pdMS_TO_TICKS(400));
    }
    s_connected = false;
    s_uplink = false;
    s_tx_inhibit = false;
    s_playing = false;
    s_start_pending = false;
    s_await_downlink = false;
    s_drain_then_close = false;
    s_ws_dead = false;
    s_play_primed = false;
    s_followup_armed = false;
    s_skip_followup = false;
    s_uplink_expect_speech = false;
    s_speech_preheard = false;
    s_followup_deadline_us = 0;
    s_nospeech_deadline_us = 0;
    s_send_fail_streak = 0;
    drain_rb(s_tx_rb);
    drain_rb(s_rx_rb);
    voice_idle();
}

static void arm_downlink_wait(void)
{
    /* 静默看门狗：任意下行活动（processing/pcm/play）都续命；
     * 真正结束等待靠 play_stop 排空，而非「猜」后端要多久。 */
    s_await_downlink = true;
    s_downlink_deadline_us =
        esp_timer_get_time() + (int64_t)CONFIG_AUDIO_WS_DOWNLINK_WAIT_MS * 1000LL;
}

static void clear_followup(void)
{
    s_followup_armed = false;
    s_followup_deadline_us = 0;
}

static void send_dialog_idle(void)
{
    /* 播完后可能仍标着 listen_stop 的 tx_inhibit，dialog_idle 必须能发出 */
    s_tx_inhibit = false;
    send_text_op("{\"op\":\"dialog_idle\"}");
}

static void do_dialog_idle(void)
{
    clear_followup();
    s_uplink_expect_speech = false;
    s_nospeech_deadline_us = 0;
    if (s_connected) {
        send_dialog_idle();
    }
    voice_idle();
    ESP_LOGI(TAG, "dialog_idle (need wake for next turn)");
}

static void arm_followup_or_idle(void)
{
    s_tx_inhibit = false;
    if (s_skip_followup) {
        s_skip_followup = false;
        ESP_LOGI(TAG, "dialog_end — skip followup");
        do_dialog_idle();
        return;
    }
    if (CONFIG_AUDIO_WS_FOLLOWUP_MS > 0 && s_connected) {
        s_followup_armed = true;
        s_followup_deadline_us =
            esp_timer_get_time() + (int64_t)CONFIG_AUDIO_WS_FOLLOWUP_MS * 1000LL;
        voice_listen();
        ESP_LOGI(TAG, "followup armed (%d ms)", CONFIG_AUDIO_WS_FOLLOWUP_MS);
    } else {
        do_dialog_idle();
    }
}

static void do_followup_nospeech_abort(void)
{
    ESP_LOGI(TAG, "followup uplink no-speech — abort");
    s_start_pending = false;
    s_uplink_expect_speech = false;
    s_nospeech_deadline_us = 0;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) == pdTRUE) {
        stop_uplink_locked("manual");
        xSemaphoreGive(s_lock);
    } else {
        s_uplink = false;
    }
    s_await_downlink = false;
    s_playing = false;
    s_drain_then_close = false;
    do_dialog_idle();
}

static void begin_uplink(void)
{
    const bool from_followup = s_followup_armed;
    clear_followup();
    drain_rb(s_tx_rb);
    drain_rb(s_rx_rb);
    s_deadline_us = esp_timer_get_time() + (int64_t)CONFIG_AUDIO_WS_MAX_MS * 1000LL;
    s_tx_origin_us = esp_timer_get_time();
    s_tx_samples_sent = 0;
    s_send_fail_streak = 0;
    s_playing = false;
    s_await_downlink = false;
    s_drain_then_close = false;
    s_tx_inhibit = false;
    s_uplink = true;
    s_play_primed = false;
    s_rx_bytes_total = 0;
    /* 续听/唤醒开上行后若迟迟无有效语音，用 FOLLOWUP 窗口避免空挂 ASR */
    if (CONFIG_AUDIO_WS_FOLLOWUP_MS > 0 && !s_speech_preheard) {
        s_uplink_expect_speech = true;
        s_nospeech_deadline_us =
            esp_timer_get_time() + (int64_t)CONFIG_AUDIO_WS_FOLLOWUP_MS * 1000LL;
    } else {
        s_uplink_expect_speech = false;
        s_nospeech_deadline_us = 0;
    }
    s_speech_preheard = false;
    (void)from_followup;
    voice_listen();
    send_text_op("{\"op\":\"uplink_start\",\"rate\":16000,\"format\":\"s16le\",\"channels\":1}");
    ESP_LOGI(TAG, "uplink started");
}

static void pace_realtime(size_t n_samples)
{
    s_tx_samples_sent += n_samples;
    const int64_t due_us =
        s_tx_origin_us + ((int64_t)s_tx_samples_sent * 1000000LL) / (int64_t)AUDIO_CAPTURE_SAMPLE_RATE;
    const int64_t now = esp_timer_get_time();
    if (due_us > now + 20000) {
        int64_t wait_ms = (due_us - now) / 1000;
        if (wait_ms > 50) {
            wait_ms = 50;
        }
        if (wait_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS((uint32_t)wait_ms));
        }
    }
}

/** 事件回调：只拷贝，不阻塞、不 send。 */
static void enqueue_downlink_pcm(const void *data, int len)
{
    if (s_rx_rb == NULL || data == NULL || len <= 0) {
        return;
    }
    size_t nbytes = (size_t)len & ~(sizeof(int16_t) - 1);
    if (nbytes == 0) {
        return;
    }
    /* 播放中尽量不丢包：满则丢最旧腾地方；丢新帧会造成长音频「播不完/跳字」 */
    UBaseType_t waiting = 0;
    vRingbufferGetInfo(s_rx_rb, NULL, NULL, NULL, NULL, &waiting);
    if ((size_t)waiting + nbytes > WS_RX_RB_BYTES) {
        make_room(s_rx_rb, WS_RX_RB_BYTES, nbytes);
    }
    if (xRingbufferSend(s_rx_rb, data, nbytes, 0) == pdTRUE) {
        size_t prev = s_rx_bytes_total;
        s_rx_bytes_total += nbytes;
        /* 首包，以及大约每 0.5s 音频打一次，确认下行 PCM 真到了 */
        if (prev == 0 || (s_rx_bytes_total / 16000) != (prev / 16000)) {
            ESP_LOGI(TAG, "<< pcm +%u total=%u", (unsigned)nbytes, (unsigned)s_rx_bytes_total);
        }
    } else {
        ESP_LOGW(TAG, "rx ring full, drop %d", len);
    }
}

static void ws_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "connected");
        s_connected = true;
        s_ws_dead = false;
        if (s_start_pending) {
            s_start_pending = false;
            /* 禁止在事件回调里 send_text（会死锁） */
            post_cmd(WS_CMD_BEGIN_UPLINK, NULL);
        }
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        if (s_uplink) {
            ESP_LOGW(TAG, "UNEXPECTED disconnect during uplink");
            s_cooldown_until_us =
                esp_timer_get_time() + (int64_t)WS_RECONNECT_COOLDOWN_MS * 1000LL;
        } else if (s_await_downlink || s_playing || s_drain_then_close) {
            ESP_LOGW(TAG, "UNEXPECTED disconnect while awaiting/playing downlink");
            s_cooldown_until_us =
                esp_timer_get_time() + (int64_t)WS_RECONNECT_COOLDOWN_MS * 1000LL;
        } else {
            ESP_LOGW(TAG, "disconnected");
        }
        s_connected = false;
        s_ws_dead = true;
        s_uplink = false;
        s_tx_inhibit = false;
        s_playing = false;
        s_start_pending = false;
        s_await_downlink = false;
        s_drain_then_close = false;
        drain_rb(s_tx_rb);
        drain_rb(s_rx_rb);
        voice_idle();
        break;

    case WEBSOCKET_EVENT_DATA:
        if (data == NULL || data->data_ptr == NULL || data->data_len <= 0) {
            break;
        }
        if (data->op_code == 0x01) { /* text */
            char buf[160];
            int n = data->data_len < (int)sizeof(buf) - 1 ? data->data_len : (int)sizeof(buf) - 1;
            memcpy(buf, data->data_ptr, n);
            buf[n] = '\0';
            cJSON *root = cJSON_Parse(buf);
            if (root == NULL) {
                break;
            }
            const cJSON *op = cJSON_GetObjectItem(root, "op");
            if (cJSON_IsString(op) && op->valuestring != NULL) {
                if (strcmp(op->valuestring, "processing") == 0) {
                    /* 网关处理中心跳：只续命静默看门狗，保持 busy，禁止再上行 */
                    s_tx_inhibit = true;
                    s_uplink = false;
                    arm_downlink_wait();
                    voice_process();
                    ESP_LOGD(TAG, "<< processing — refresh await");
                } else if (strcmp(op->valuestring, "listen_stop") == 0 ||
                           strcmp(op->valuestring, "transcript") == 0 ||
                           strcmp(op->valuestring, "reply") == 0) {
                    s_tx_inhibit = true;
                    s_uplink = false;
                    drain_rb(s_tx_rb);
                    arm_downlink_wait();
                    voice_process();
                    ESP_LOGI(TAG, "<< %s — force stop uplink, await downlink", op->valuestring);
                    post_cmd(WS_CMD_LISTEN_STOP, "asr");
                } else if (strcmp(op->valuestring, "error") == 0) {
                    /* 网关 busy 等：停上行，勿继续 send_bin */
                    s_tx_inhibit = true;
                    s_uplink = false;
                    drain_rb(s_tx_rb);
                    ESP_LOGW(TAG, "<< error — stop uplink");
                } else if (strcmp(op->valuestring, "play_start") == 0) {
                    /* 立刻停上行标志，避免 worker 继续 send_bin 抢锁 */
                    s_tx_inhibit = true;
                    s_uplink = false;
                    drain_rb(s_tx_rb);
                    s_playing = true;
                    s_drain_then_close = false;
                    s_play_primed = false;
                    s_rx_bytes_total = 0;
                    arm_downlink_wait();
                    voice_play();
                    ESP_LOGI(TAG, "<< play_start");
                    post_cmd(WS_CMD_PLAY_START, "play");
                } else if (strcmp(op->valuestring, "play_stop") == 0) {
                    /* 勿立刻 CLOSE：先把 rx 里剩余 PCM 播完 */
                    ESP_LOGI(TAG, "<< play_stop (rx_total=%u) — drain keep ws",
                             (unsigned)s_rx_bytes_total);
                    s_drain_then_close = true;
                    s_playing = true;
                    post_cmd(WS_CMD_CLOSE, NULL);
                } else if (strcmp(op->valuestring, "dialog_end") == 0) {
                    /* 用户口令退出：播完后不进入续听 */
                    s_skip_followup = true;
                    ESP_LOGI(TAG, "<< dialog_end — no followup after play");
                    if (s_followup_armed && !s_playing && !s_drain_then_close && !s_uplink) {
                        post_cmd(WS_CMD_DIALOG_IDLE, NULL);
                    }
                }
            }
            cJSON_Delete(root);
        } else if (data->op_code == 0x02 || data->op_code == 0x00) {
            /* 0x02 binary / 0x00 continuation → ring，勿在此写 I2S */
            s_tx_inhibit = true;
            s_uplink = false;
            if (!s_playing) {
                s_playing = true;
                s_drain_then_close = false;
                s_play_primed = false;
                s_rx_bytes_total = 0;
                voice_play();
                post_cmd(WS_CMD_PLAY_START, "play");
            }
            enqueue_downlink_pcm(data->data_ptr, data->data_len);
            arm_downlink_wait();
        }
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "websocket error");
        /* 连接尚未建成就失败：保留 start_pending，由 worker 重试 */
        s_uplink = false;
        s_tx_inhibit = false;
        s_playing = false;
        s_await_downlink = false;
        s_drain_then_close = false;
        s_ws_dead = true;
        if (!s_start_pending) {
            s_connected = false;
        }
        voice_idle();
        break;

    default:
        break;
    }
}

static const char *active_ws_uri(void)
{
    const char *uri = CONFIG_AUDIO_WS_URI;
#if CONFIG_MCP_GW_CLIENT_ENABLE
    const char *discovered = mcp_gw_get_voice_ws_uri();
    if (discovered && discovered[0]) {
        uri = discovered;
    }
#endif
    return uri;
}

static esp_err_t ensure_client_started(void)
{
    if (s_client != NULL && !s_ws_dead && esp_websocket_client_is_connected(s_client)) {
        s_connected = true;
        return ESP_OK;
    }

    if (s_uplink) {
        ESP_LOGW(TAG, "skip reconnect: uplink active");
        return ESP_ERR_INVALID_STATE;
    }

    destroy_client();

    const char *uri = active_ws_uri();
    ESP_LOGI(TAG, "using gateway URI: %s", uri);

    /* 栈/缓冲过大或 internal 碎片时 xTaskCreate 会失败（Error create websocket task） */
    esp_websocket_client_config_t cfg = {
        .uri = uri,
        .buffer_size = WS_BUFFER_SIZE,
        .task_stack = WS_TASK_STACK,
        .task_prio = WS_TASK_PRIO,
        .disable_auto_reconnect = true,
        .disable_pingpong_discon = true,
        .network_timeout_ms = 30000,
        /* 禁用主动 ping：长时间 TTS 时 ping 与收包争用易触发 abort；靠业务帧保活 */
        .ping_interval_sec = 1000000000,
        .keep_alive_enable = false,
    };

    if (CONFIG_AUDIO_WS_AUTH_HEADER[0] != '\0') {
        snprintf(s_auth_headers, sizeof(s_auth_headers), "%s\r\n", CONFIG_AUDIO_WS_AUTH_HEADER);
        cfg.headers = s_auth_headers;
    }

    for (int attempt = 1; attempt <= 3; ++attempt) {
        size_t heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
        size_t intern = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        ESP_LOGI(TAG, "ws connect attempt %d free_heap=%u internal=%u largest_int=%u",
                 attempt, (unsigned)heap, (unsigned)intern, (unsigned)largest);
        if (largest < (size_t)WS_TASK_STACK + 1024) {
            ESP_LOGW(TAG, "internal heap fragmented — wait before retry");
            vTaskDelay(pdMS_TO_TICKS(500 * attempt));
        }

        s_client = esp_websocket_client_init(&cfg);
        if (s_client == NULL) {
            ESP_LOGE(TAG, "ws init failed (no mem)");
            vTaskDelay(pdMS_TO_TICKS(300 * attempt));
            continue;
        }
        esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL);
        esp_err_t err = esp_websocket_client_start(s_client);
        if (err == ESP_OK) {
            /* start 只是启动任务；等一会看是否真连上，失败则重建 */
            for (int w = 0; w < 50; ++w) {
                if (s_connected || esp_websocket_client_is_connected(s_client)) {
                    s_connected = true;
                    return ESP_OK;
                }
                if (s_client == NULL) {
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            if (esp_websocket_client_is_connected(s_client)) {
                s_connected = true;
                return ESP_OK;
            }
            ESP_LOGW(TAG, "ws start ok but not connected yet (attempt %d)", attempt);
            esp_websocket_client_stop(s_client);
            esp_websocket_client_destroy(s_client);
            s_client = NULL;
            s_connected = false;
            vTaskDelay(pdMS_TO_TICKS(400 * attempt));
            continue;
        }
        ESP_LOGE(TAG, "ws start failed: %s (heap=%u)", esp_err_to_name(err), (unsigned)heap);
        esp_websocket_client_destroy(s_client);
        s_client = NULL;
        vTaskDelay(pdMS_TO_TICKS(400 * attempt));
    }
    return ESP_FAIL;
}

static void do_session_start(void)
{
    /* 网关重启后旧连接已死，但 disable_auto_reconnect 不会自愈；
     * 若仍标 busy/playing，会永久忽略唤醒 —— 先探测并清僵尸状态。 */
    if (s_ws_dead || (s_client != NULL && !esp_websocket_client_is_connected(s_client))) {
        ESP_LOGW(TAG, "stale/dead ws — reset session flags and reconnect");
        s_connected = false;
        s_uplink = false;
        s_tx_inhibit = false;
        s_playing = false;
        s_start_pending = false;
        s_await_downlink = false;
        s_drain_then_close = false;
        s_cooldown_until_us = 0;
        clear_followup();
        s_uplink_expect_speech = false;
        drain_rb(s_tx_rb);
        drain_rb(s_rx_rb);
        destroy_client();
    }

    if (s_uplink) {
        s_deadline_us = esp_timer_get_time() + (int64_t)CONFIG_AUDIO_WS_MAX_MS * 1000LL;
        ESP_LOGI(TAG, "uplink already active — refresh deadline");
        return;
    }

    if (s_playing || s_await_downlink || s_drain_then_close) {
        ESP_LOGI(TAG, "busy (play/await downlink) — ignore new session_start");
        return;
    }

    if (esp_timer_get_time() < s_cooldown_until_us) {
        ESP_LOGW(TAG, "reconnect cooldown — ignore session_start");
        return;
    }

    esp_err_t err = ensure_client_started();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "connect failed: %s", esp_err_to_name(err));
        voice_idle();
        return;
    }

    if (s_connected) {
        begin_uplink();
    } else {
        s_start_pending = true;
        voice_listen();
        ESP_LOGI(TAG, "connecting to %s …", active_ws_uri());
    }
}

static void do_uplink_eos(const char *reason)
{
    s_start_pending = false;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) == pdTRUE) {
        stop_uplink_locked(reason);
        xSemaphoreGive(s_lock);
    } else {
        s_uplink = false;
    }
    if (s_connected) {
        arm_downlink_wait();
        voice_process();
        ESP_LOGI(TAG, "await downlink (keep ws, silence_wd=%dms)", CONFIG_AUDIO_WS_DOWNLINK_WAIT_MS);
    }
}

static void do_play_start(const char *reason)
{
    /* 若仍在上行（网关抢先下发），补发 uplink_stop；暂不 enable 功放，等预缓冲满 */
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) == pdTRUE) {
        if (s_uplink) {
            stop_uplink_locked(reason && reason[0] ? reason : "play");
        } else {
            drain_rb(s_tx_rb);
        }
        xSemaphoreGive(s_lock);
    } else {
        s_uplink = false;
    }
    s_playing = true;
    s_play_primed = false;
    arm_downlink_wait();
    voice_play();
    if (!ns4168_amp_is_ready()) {
        ESP_LOGE(TAG, "amp not ready — cannot play downlink");
        return;
    }
    ESP_LOGI(TAG, "play arming, wait prebuffer=%u bytes before amp on",
             (unsigned)WS_PREBUFFER_BYTES);
}

static void do_listen_stop(const char *reason)
{
    /* 事件回调里已把 s_uplink 清掉；这里只清 TX，绝不 send（写失败会 abort 整条连接） */
    (void)reason;
    s_tx_inhibit = true;
    s_uplink = false;
    drain_rb(s_tx_rb);
    if (s_connected) {
        arm_downlink_wait();
        voice_process();
    }
    ESP_LOGI(TAG, "listen_stop done, await downlink (tx inhibited)");
}

static size_t rx_waiting_bytes(void)
{
    if (s_rx_rb == NULL) {
        return 0;
    }
    UBaseType_t waiting = 0;
    vRingbufferGetInfo(s_rx_rb, NULL, NULL, NULL, NULL, &waiting);
    return (size_t)waiting;
}

static bool play_from_rx(void)
{
    if (s_rx_rb == NULL || (!s_playing && !s_drain_then_close)) {
        return false;
    }
    /* 预缓冲未满时绝不开功放/写 I2S，避免开头静音→有声的「卡一下」 */
    if (!s_play_primed && !s_drain_then_close) {
        return false;
    }
    if (!ns4168_amp_is_ready()) {
        return false;
    }
    if (ns4168_amp_set_enabled(true) != ESP_OK) {
        return false;
    }

    uint8_t chunk[WS_PLAY_CHUNK];
    size_t item_size = 0;
    void *item = xRingbufferReceiveUpTo(s_rx_rb, &item_size, 0, sizeof(chunk));
    if (item == NULL) {
        return false;
    }
    size_t n = item_size > sizeof(chunk) ? sizeof(chunk) : item_size;
    memcpy(chunk, item, n);
    vRingbufferReturnItem(s_rx_rb, item);

    size_t samples = n / sizeof(int16_t);
    if (samples == 0) {
        return true;
    }
    esp_err_t err = ns4168_amp_write((const int16_t *)chunk, samples);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "amp_write: %s", esp_err_to_name(err));
    }
    return true;
}

static void do_session_stop(const char *reason)
{
    s_start_pending = false;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) == pdTRUE) {
        stop_uplink_locked(reason);
        xSemaphoreGive(s_lock);
    } else {
        s_uplink = false;
    }
    destroy_client();
}

static void worker_task(void *arg)
{
    (void)arg;
    uint8_t chunk[WS_SEND_CHUNK];
    ws_cmd_msg_t msg;

    while (s_worker_run) {
        const bool draining = s_playing || s_drain_then_close;
        if (xQueueReceive(s_cmd_q, &msg, draining ? 0 : pdMS_TO_TICKS(10)) == pdTRUE) {
            if (msg.cmd == WS_CMD_SESSION_START) {
                do_session_start();
            } else if (msg.cmd == WS_CMD_BEGIN_UPLINK) {
                begin_uplink();
            } else if (msg.cmd == WS_CMD_EOS) {
                do_uplink_eos(msg.reason[0] ? msg.reason : "vad");
            } else if (msg.cmd == WS_CMD_PLAY_START) {
                do_play_start(msg.reason);
            } else if (msg.cmd == WS_CMD_LISTEN_STOP) {
                do_listen_stop(msg.reason);
            } else if (msg.cmd == WS_CMD_DIALOG_IDLE) {
                do_dialog_idle();
            } else if (msg.cmd == WS_CMD_FOLLOWUP_ABORT) {
                do_followup_nospeech_abort();
            } else if (msg.cmd == WS_CMD_SESSION_STOP) {
                do_session_stop(msg.reason[0] ? msg.reason : "manual");
            } else if (msg.cmd == WS_CMD_CLOSE) {
                /* play_stop：播完 rx 后保持连接，供下一轮 uplink_start 复用 */
                if (!s_uplink) {
                    s_drain_then_close = true;
                    s_playing = true;
                }
            }
        }

        /* 下行播放：攒满预缓冲 → 开功放；按 I2S 阻塞节拍单帧写，避免追赶塞爆 DMA */
        if (s_playing || s_drain_then_close) {
            if (s_playing && !s_drain_then_close && !s_play_primed) {
                size_t waiting = rx_waiting_bytes();
                if (waiting < WS_PREBUFFER_BYTES) {
                    vTaskDelay(pdMS_TO_TICKS(5));
                    continue;
                }
                if (ns4168_amp_set_enabled(true) != ESP_OK) {
                    ESP_LOGE(TAG, "amp enable at prime failed");
                    vTaskDelay(pdMS_TO_TICKS(20));
                    continue;
                }
                s_play_primed = true;
                ESP_LOGI(TAG, "play primed amp on (%u bytes)", (unsigned)waiting);
            }

            if (!play_from_rx()) {
                if (s_playing || s_drain_then_close) {
                    vTaskDelay(pdMS_TO_TICKS(5));
                }
            }
        }

        if (s_drain_then_close && !s_uplink) {
            UBaseType_t waiting = 0;
            if (s_rx_rb != NULL) {
                vRingbufferGetInfo(s_rx_rb, NULL, NULL, NULL, NULL, &waiting);
            }
            if (waiting == 0) {
                ESP_LOGI(TAG, "downlink done, keep ws (pcm_total=%u)",
                         (unsigned)s_rx_bytes_total);
                if (s_rx_bytes_total == 0) {
                    ESP_LOGW(TAG, "no downlink PCM received — check gateway binary frames");
                }
                s_playing = false;
                s_drain_then_close = false;
                s_await_downlink = false;
                s_play_primed = false;
                (void)ns4168_amp_set_enabled(false);
                /* 故意不 destroy：避免二次建连任务失败 / 下行被 RST 掐断 */
                arm_followup_or_idle();
                continue;
            }
        }

        if (s_followup_armed && !s_uplink && !s_playing && !s_await_downlink &&
            !s_drain_then_close && esp_timer_get_time() >= s_followup_deadline_us) {
            ESP_LOGI(TAG, "followup timeout");
            do_dialog_idle();
            continue;
        }

        if (s_uplink && s_uplink_expect_speech && s_nospeech_deadline_us > 0 &&
            esp_timer_get_time() >= s_nospeech_deadline_us) {
            do_followup_nospeech_abort();
            continue;
        }

        if (s_await_downlink && !s_uplink && s_connected && !s_drain_then_close && !s_playing &&
            esp_timer_get_time() >= s_downlink_deadline_us) {
            ESP_LOGW(TAG, "downlink silence watchdog — idle (pcm_total=%u, no processing/play)",
                     (unsigned)s_rx_bytes_total);
            s_playing = false;
            s_await_downlink = false;
            s_drain_then_close = false;
            do_dialog_idle();
            continue;
        }

        if (!s_uplink || s_tx_inhibit || s_client == NULL || !s_connected || s_playing) {
            continue;
        }

        if (esp_timer_get_time() >= s_deadline_us) {
            ESP_LOGI(TAG, "max duration — stop uplink");
            do_uplink_eos("max");
            continue;
        }

        size_t item_size = 0;
        void *item = xRingbufferReceiveUpTo(s_tx_rb, &item_size, 0, sizeof(chunk));
        if (item == NULL) {
            continue;
        }
        size_t n = item_size > sizeof(chunk) ? sizeof(chunk) : item_size;
        memcpy(chunk, item, n);
        vRingbufferReturnItem(s_tx_rb, item);

        if (!(s_uplink && !s_tx_inhibit && s_connected && s_client != NULL) || s_playing) {
            continue;
        }

        pace_realtime(n / sizeof(int16_t));

        /* pace 期间可能已收到 listen_stop / play_start */
        if (!(s_uplink && !s_tx_inhibit && s_connected && !s_playing) || s_client == NULL) {
            continue;
        }
        if (!esp_websocket_client_is_connected(s_client)) {
            s_connected = false;
            s_ws_dead = true;
            s_uplink = false;
            drain_rb(s_tx_rb);
            ESP_LOGW(TAG, "ws not connected — stop uplink");
            continue;
        }

        int ret = esp_websocket_client_send_bin(s_client, (const char *)chunk, (int)n,
                                                pdMS_TO_TICKS(WS_SEND_TIMEOUT_MS));
        if (ret <= 0) {
            /* 组件写失败常会 abort 连接。勿在此处立刻 destroy（阻塞 worker、碎片化），
             * 标死连接，等下一轮 session_start 再干净重建。 */
            ESP_LOGW(TAG, "send_bin failed (%d) — mark dead, defer reconnect", ret);
            s_uplink = false;
            s_tx_inhibit = true;
            s_connected = false;
            s_ws_dead = true;
            s_playing = false;
            s_await_downlink = false;
            s_drain_then_close = false;
            drain_rb(s_tx_rb);
            drain_rb(s_rx_rb);
            s_cooldown_until_us = esp_timer_get_time() + 1500LL * 1000LL;
            voice_idle();
            continue;
        }
        s_send_fail_streak = 0;
    }

    destroy_client();
    s_worker = NULL;
    vTaskDeleteWithCaps(NULL);
}

static void post_cmd(ws_cmd_t cmd, const char *reason)
{
    if (s_cmd_q == NULL) {
        return;
    }
    ws_cmd_msg_t msg = {.cmd = cmd};
    memset(msg.reason, 0, sizeof(msg.reason));
    if (reason != NULL) {
        strncpy(msg.reason, reason, sizeof(msg.reason) - 1);
    }
    (void)xQueueSend(s_cmd_q, &msg, 0);
}

esp_err_t audio_ws_client_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    s_lock = xSemaphoreCreateMutex();
    s_cmd_q = xQueueCreate(WS_CMD_QUEUE_LEN, sizeof(ws_cmd_msg_t));
    /* 大 ring 放 PSRAM，避免吃光 internal 导致 websocket 任务创建失败 */
    s_tx_rb = xRingbufferCreateWithCaps(WS_TX_RB_BYTES, RINGBUF_TYPE_BYTEBUF,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_tx_rb == NULL) {
        s_tx_rb = xRingbufferCreateWithCaps(WS_TX_RB_BYTES, RINGBUF_TYPE_BYTEBUF,
                                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    s_rx_rb = xRingbufferCreateWithCaps(WS_RX_RB_BYTES, RINGBUF_TYPE_BYTEBUF,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_rx_rb == NULL) {
        s_rx_rb = xRingbufferCreateWithCaps(WS_RX_RB_BYTES, RINGBUF_TYPE_BYTEBUF,
                                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (s_lock == NULL || s_cmd_q == NULL || s_tx_rb == NULL || s_rx_rb == NULL) {
        audio_ws_client_deinit();
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = audio_capture_acquire();
    if (err != ESP_OK) {
        audio_ws_client_deinit();
        return err;
    }

    err = audio_capture_add_sink(audio_ws_client_on_pcm, NULL, &s_sink_id);
    if (err != ESP_OK) {
        audio_capture_release();
        audio_ws_client_deinit();
        return err;
    }

    s_worker_run = true;
    /* worker 栈放 PSRAM，给 SDIO/WiFi/网页留 internal */
    if (xTaskCreateWithCaps(worker_task, "aud_ws", WS_WORKER_STACK, NULL, WS_WORKER_PRIO,
                            &s_worker, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        if (xTaskCreateWithCaps(worker_task, "aud_ws", WS_WORKER_STACK, NULL, WS_WORKER_PRIO,
                                &s_worker, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
            s_worker_run = false;
            audio_capture_remove_sink(s_sink_id);
            s_sink_id = -1;
            audio_capture_release();
            audio_ws_client_deinit();
            return ESP_ERR_NO_MEM;
        }
    }

    s_inited = true;
    ESP_LOGI(TAG, "init OK sink=%d uri=%s (fallback) eos=%dms max=%dms dl_wait=%dms followup=%dms",
             s_sink_id, CONFIG_AUDIO_WS_URI,
             CONFIG_AUDIO_WS_EOS_MS, CONFIG_AUDIO_WS_MAX_MS,
             CONFIG_AUDIO_WS_DOWNLINK_WAIT_MS, CONFIG_AUDIO_WS_FOLLOWUP_MS);
    return ESP_OK;
}

void audio_ws_client_deinit(void)
{
    s_inited = false;
    if (s_cmd_q != NULL) {
        post_cmd(WS_CMD_SESSION_STOP, "manual");
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    s_worker_run = false;

    while (s_worker != NULL) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    destroy_client();

    if (s_sink_id >= 0) {
        audio_capture_remove_sink(s_sink_id);
        s_sink_id = -1;
        audio_capture_release();
    }
    if (s_tx_rb != NULL) {
        vRingbufferDeleteWithCaps(s_tx_rb);
        s_tx_rb = NULL;
    }
    if (s_rx_rb != NULL) {
        vRingbufferDeleteWithCaps(s_rx_rb);
        s_rx_rb = NULL;
    }
    if (s_cmd_q != NULL) {
        vQueueDelete(s_cmd_q);
        s_cmd_q = NULL;
    }
    if (s_lock != NULL) {
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
    }
}

esp_err_t audio_ws_client_session_start(void)
{
    if (!s_inited) {
        esp_err_t err = audio_ws_client_init();
        if (err != ESP_OK) {
            return err;
        }
    }
    post_cmd(WS_CMD_SESSION_START, NULL);
    return ESP_OK;
}

void audio_ws_client_session_stop(const char *reason)
{
    post_cmd(WS_CMD_SESSION_STOP, reason ? reason : "manual");
}

void audio_ws_client_notify_eos(void)
{
    if (!s_uplink || s_tx_inhibit) {
        return;
    }
    post_cmd(WS_CMD_EOS, "vad");
}

bool audio_ws_client_is_uplink_active(void)
{
    return s_uplink;
}

bool audio_ws_client_is_playing(void)
{
    return s_playing;
}

bool audio_ws_client_is_busy(void)
{
    /* 连接已死时不要把旧的 play/await 当成 busy，否则唤醒永远进不来 */
    if (s_client == NULL || !esp_websocket_client_is_connected(s_client)) {
        return s_uplink || s_start_pending;
    }
    /* followup armed 不算 busy：允许唤醒或 VAD 直接开上行 */
    return s_uplink || s_playing || s_await_downlink || s_start_pending || s_drain_then_close;
}

bool audio_ws_client_is_followup_armed(void)
{
    return s_followup_armed && CONFIG_AUDIO_WS_FOLLOWUP_MS > 0;
}

esp_err_t audio_ws_client_followup_begin(void)
{
    return audio_ws_client_session_start();
}

void audio_ws_client_followup_nospeech_abort(void)
{
    if (!s_uplink || !s_uplink_expect_speech) {
        return;
    }
    post_cmd(WS_CMD_FOLLOWUP_ABORT, "manual");
}

void audio_ws_client_notify_speech_heard(void)
{
    s_uplink_expect_speech = false;
    s_nospeech_deadline_us = 0;
    s_speech_preheard = true;
}

void audio_ws_client_on_pcm(const int16_t *pcm, size_t n_samples, void *ctx)
{
    (void)ctx;
    if (!s_uplink || s_tx_inhibit || s_playing || s_tx_rb == NULL || pcm == NULL || n_samples == 0) {
        return;
    }
    if (esp_timer_get_time() >= s_deadline_us) {
        return;
    }
    const size_t nbytes = n_samples * sizeof(int16_t);
    make_room(s_tx_rb, WS_TX_RB_BYTES, nbytes);
    (void)xRingbufferSend(s_tx_rb, pcm, nbytes, 0);
}

#else /* !CONFIG_AUDIO_WS_ENABLE */

esp_err_t audio_ws_client_init(void)
{
    return ESP_OK;
}

void audio_ws_client_deinit(void) {}

esp_err_t audio_ws_client_session_start(void)
{
    ESP_LOGW(TAG, "AUDIO_WS_ENABLE is off");
    return ESP_ERR_NOT_SUPPORTED;
}

void audio_ws_client_session_stop(const char *reason)
{
    (void)reason;
}

void audio_ws_client_notify_eos(void) {}

bool audio_ws_client_is_uplink_active(void)
{
    return false;
}

bool audio_ws_client_is_playing(void)
{
    return false;
}

bool audio_ws_client_is_busy(void)
{
    return false;
}

bool audio_ws_client_is_followup_armed(void)
{
    return false;
}

esp_err_t audio_ws_client_followup_begin(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

void audio_ws_client_followup_nospeech_abort(void) {}

void audio_ws_client_notify_speech_heard(void) {}

void audio_ws_client_on_pcm(const int16_t *pcm, size_t n_samples, void *ctx)
{
    (void)pcm;
    (void)n_samples;
    (void)ctx;
}

#endif /* CONFIG_AUDIO_WS_ENABLE */
