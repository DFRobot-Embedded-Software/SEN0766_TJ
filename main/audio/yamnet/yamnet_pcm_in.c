/**
 * YAMNet PCM 输入桥：私有 ring + audio_capture sink（不碰硬件）。
 */
#include "yamnet_pcm_in.h"
#include "audio_capture.h"

#include <string.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"

static const char *TAG = "yamnet_pcm";

#define PCM_RING_BYTES (AUDIO_CAPTURE_SAMPLE_RATE * sizeof(int16_t))

static RingbufHandle_t s_pcm_rb;
static int s_sink_id = -1;
static bool s_started;

/** ring 将满时丢弃最旧数据，为 need_bytes 腾出空间。 */
static void make_room(size_t need_bytes)
{
    if (s_pcm_rb == NULL) {
        return;
    }
    for (;;) {
        UBaseType_t waiting = 0;
        vRingbufferGetInfo(s_pcm_rb, NULL, NULL, NULL, NULL, &waiting);
        if ((size_t)waiting + need_bytes <= PCM_RING_BYTES) {
            break;
        }
        size_t item_size = 0;
        void *item = xRingbufferReceiveUpTo(s_pcm_rb, &item_size, 0, need_bytes);
        if (item == NULL) {
            break;
        }
        vRingbufferReturnItem(s_pcm_rb, item);
    }
}

/** capture sink：增益后 PCM 写入本模块私有 ring。 */
static void on_capture_pcm(const int16_t *pcm, size_t n_samples, void *ctx)
{
    (void)ctx;
    if (s_pcm_rb == NULL || pcm == NULL || n_samples == 0) {
        return;
    }
    const size_t nbytes = n_samples * sizeof(int16_t);
    make_room(nbytes);
    (void)xRingbufferSend(s_pcm_rb, pcm, nbytes, 0);
}

esp_err_t yamnet_pcm_in_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    s_pcm_rb = xRingbufferCreateWithCaps(PCM_RING_BYTES, RINGBUF_TYPE_BYTEBUF,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_pcm_rb == NULL) {
        s_pcm_rb = xRingbufferCreateWithCaps(PCM_RING_BYTES, RINGBUF_TYPE_BYTEBUF,
                                             MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (s_pcm_rb == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = audio_capture_acquire();
    if (err != ESP_OK) {
        vRingbufferDeleteWithCaps(s_pcm_rb);
        s_pcm_rb = NULL;
        return err;
    }

    err = audio_capture_add_sink(on_capture_pcm, NULL, &s_sink_id);
    if (err != ESP_OK) {
        audio_capture_release();
        vRingbufferDeleteWithCaps(s_pcm_rb);
        s_pcm_rb = NULL;
        return err;
    }

    s_started = true;
    ESP_LOGI(TAG, "YAMNet PCM bridge up (sink=%d)", s_sink_id);
    return ESP_OK;
}

void yamnet_pcm_in_stop(void)
{
    if (!s_started) {
        return;
    }
    s_started = false;

    if (s_sink_id >= 0) {
        audio_capture_remove_sink(s_sink_id);
        s_sink_id = -1;
    }
    audio_capture_release();

    if (s_pcm_rb != NULL) {
        vRingbufferDeleteWithCaps(s_pcm_rb);
        s_pcm_rb = NULL;
    }
}

size_t yamnet_pcm_in_read(int16_t *out, size_t max_samples)
{
    if (s_pcm_rb == NULL || out == NULL || max_samples == 0) {
        return 0;
    }

    size_t item_size = 0;
    void *item = xRingbufferReceiveUpTo(s_pcm_rb, &item_size, 0, max_samples * sizeof(int16_t));
    if (item == NULL) {
        return 0;
    }
    size_t n = item_size / sizeof(int16_t);
    memcpy(out, item, n * sizeof(int16_t));
    vRingbufferReturnItem(s_pcm_rb, item);
    return n;
}

size_t yamnet_pcm_in_bytes_pending(void)
{
    if (s_pcm_rb == NULL) {
        return 0;
    }
    UBaseType_t waiting = 0;
    vRingbufferGetInfo(s_pcm_rb, NULL, NULL, NULL, NULL, &waiting);
    return (size_t)waiting;
}

void yamnet_pcm_in_discard_bytes(size_t max_bytes)
{
    if (s_pcm_rb == NULL || max_bytes == 0) {
        return;
    }
    size_t left = max_bytes;
    while (left > 0) {
        size_t chunk = left > 2048 ? 2048 : left;
        size_t item_size = 0;
        void *item = xRingbufferReceiveUpTo(s_pcm_rb, &item_size, 0, chunk);
        if (item == NULL) {
            break;
        }
        left -= item_size;
        vRingbufferReturnItem(s_pcm_rb, item);
    }
}
