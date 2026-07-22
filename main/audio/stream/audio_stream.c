/**
 * @file audio_stream.c
 * @brief UDP PCM 透传：独立订阅 audio_capture，与 YAMNet 无直接依赖
 */
#include "audio_stream.h"
#include "audio_capture.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if CONFIG_AUDIO_STREAM_ENABLE
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#endif

static const char *TAG = "audio_stream";

#if CONFIG_AUDIO_STREAM_ENABLE

#define STREAM_RB_BYTES (8000 * sizeof(int16_t))
#define STREAM_SEND_CHUNK (512 * sizeof(int16_t))

static RingbufHandle_t s_tx_rb;
static TaskHandle_t s_tx_task;
static volatile bool s_active;
static volatile bool s_task_run;
static int s_sock = -1;
static int s_sink_id = -1;
static bool s_inited;
static struct sockaddr_in s_dest;
static int64_t s_deadline_us;

static void close_sock(void)
{
    if (s_sock >= 0) {
        close(s_sock);
        s_sock = -1;
    }
}

static esp_err_t open_sock(void)
{
    close_sock();

    memset(&s_dest, 0, sizeof(s_dest));
    s_dest.sin_family = AF_INET;
    s_dest.sin_port = htons((uint16_t)CONFIG_AUDIO_STREAM_UDP_PORT);
    if (inet_aton(CONFIG_AUDIO_STREAM_UDP_HOST, &s_dest.sin_addr) == 0) {
        ESP_LOGE(TAG, "bad AUDIO_STREAM_UDP_HOST '%s'", CONFIG_AUDIO_STREAM_UDP_HOST);
        return ESP_ERR_INVALID_ARG;
    }

    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "socket failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void drain_tx_rb(void)
{
    if (s_tx_rb == NULL) {
        return;
    }
    for (;;) {
        size_t item_size = 0;
        void *item = xRingbufferReceiveUpTo(s_tx_rb, &item_size, 0, 2048);
        if (item == NULL) {
            break;
        }
        vRingbufferReturnItem(s_tx_rb, item);
    }
}

static void make_room(size_t need_bytes)
{
    if (s_tx_rb == NULL) {
        return;
    }
    for (;;) {
        UBaseType_t waiting = 0;
        vRingbufferGetInfo(s_tx_rb, NULL, NULL, NULL, NULL, &waiting);
        if ((size_t)waiting + need_bytes <= STREAM_RB_BYTES) {
            break;
        }
        size_t item_size = 0;
        void *item = xRingbufferReceiveUpTo(s_tx_rb, &item_size, 0, need_bytes);
        if (item == NULL) {
            break;
        }
        vRingbufferReturnItem(s_tx_rb, item);
    }
}

static void tx_task(void *arg)
{
    (void)arg;
    uint8_t chunk[STREAM_SEND_CHUNK];

    while (s_task_run) {
        if (!s_active || s_sock < 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (esp_timer_get_time() >= s_deadline_us) {
            ESP_LOGI(TAG, "max duration reached — stop");
            audio_stream_stop();
            continue;
        }

        size_t item_size = 0;
        void *item = xRingbufferReceiveUpTo(s_tx_rb, &item_size, pdMS_TO_TICKS(50), sizeof(chunk));
        if (item == NULL) {
            continue;
        }
        size_t n = item_size > sizeof(chunk) ? sizeof(chunk) : item_size;
        memcpy(chunk, item, n);
        vRingbufferReturnItem(s_tx_rb, item);

        if (s_sock >= 0 && s_active) {
            (void)sendto(s_sock, chunk, n, 0, (struct sockaddr *)&s_dest, sizeof(s_dest));
        }
    }

    s_tx_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t audio_stream_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    s_tx_rb = xRingbufferCreate(STREAM_RB_BYTES, RINGBUF_TYPE_BYTEBUF);
    if (s_tx_rb == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = audio_capture_acquire();
    if (err != ESP_OK) {
        vRingbufferDelete(s_tx_rb);
        s_tx_rb = NULL;
        return err;
    }

    err = audio_capture_add_sink(audio_stream_on_pcm, NULL, &s_sink_id);
    if (err != ESP_OK) {
        audio_capture_release();
        vRingbufferDelete(s_tx_rb);
        s_tx_rb = NULL;
        return err;
    }

    s_task_run = true;
    if (xTaskCreate(tx_task, "aud_stream", 4096, NULL, 4, &s_tx_task) != pdPASS) {
        s_task_run = false;
        audio_capture_remove_sink(s_sink_id);
        s_sink_id = -1;
        audio_capture_release();
        vRingbufferDelete(s_tx_rb);
        s_tx_rb = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_inited = true;
    ESP_LOGI(TAG, "init OK sink=%d UDP %s:%d max %d ms",
             s_sink_id,
             CONFIG_AUDIO_STREAM_UDP_HOST, CONFIG_AUDIO_STREAM_UDP_PORT,
             CONFIG_AUDIO_STREAM_MAX_MS);
    return ESP_OK;
}

void audio_stream_deinit(void)
{
    if (!s_inited) {
        return;
    }
    s_inited = false;

    audio_stream_stop();
    s_task_run = false;
    while (s_tx_task != NULL) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    if (s_sink_id >= 0) {
        audio_capture_remove_sink(s_sink_id);
        s_sink_id = -1;
    }
    audio_capture_release();

    if (s_tx_rb != NULL) {
        vRingbufferDelete(s_tx_rb);
        s_tx_rb = NULL;
    }
}

esp_err_t audio_stream_start(void)
{
    if (!s_inited) {
        esp_err_t err = audio_stream_init();
        if (err != ESP_OK) {
            return err;
        }
    }
    if (s_active) {
        s_deadline_us = esp_timer_get_time() + (int64_t)CONFIG_AUDIO_STREAM_MAX_MS * 1000;
        return ESP_OK;
    }

    esp_err_t err = open_sock();
    if (err != ESP_OK) {
        return err;
    }

    drain_tx_rb();

    s_deadline_us = esp_timer_get_time() + (int64_t)CONFIG_AUDIO_STREAM_MAX_MS * 1000;
    s_active = true;
    ESP_LOGI(TAG, "STREAMING → udp://%s:%d", CONFIG_AUDIO_STREAM_UDP_HOST, CONFIG_AUDIO_STREAM_UDP_PORT);
    return ESP_OK;
}

void audio_stream_stop(void)
{
    if (!s_active) {
        close_sock();
        return;
    }
    s_active = false;
    close_sock();
    ESP_LOGI(TAG, "stream stopped");
}

bool audio_stream_is_active(void)
{
    return s_active;
}

void audio_stream_on_pcm(const int16_t *pcm, size_t n_samples)
{
    if (!s_active || s_tx_rb == NULL || pcm == NULL || n_samples == 0) {
        return;
    }
    if (esp_timer_get_time() >= s_deadline_us) {
        audio_stream_stop();
        return;
    }

    const size_t nbytes = n_samples * sizeof(int16_t);
    make_room(nbytes);
    (void)xRingbufferSend(s_tx_rb, pcm, nbytes, 0);
}

#else /* !CONFIG_AUDIO_STREAM_ENABLE */

esp_err_t audio_stream_init(void)
{
    return ESP_OK;
}

void audio_stream_deinit(void) {}

esp_err_t audio_stream_start(void)
{
    ESP_LOGW(TAG, "AUDIO_STREAM_ENABLE is off");
    return ESP_ERR_NOT_SUPPORTED;
}

void audio_stream_stop(void) {}

bool audio_stream_is_active(void)
{
    return false;
}

void audio_stream_on_pcm(const int16_t *pcm, size_t n_samples)
{
    (void)pcm;
    (void)n_samples;
}

#endif /* CONFIG_AUDIO_STREAM_ENABLE */
