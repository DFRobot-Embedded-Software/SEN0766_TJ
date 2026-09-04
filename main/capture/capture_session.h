/**
 * @file capture_session.h
 * @brief Web 采集会话：PCM 流 + 1 Hz 环境/雷达采样（不上板存盘）
 */
#pragma once

#include "cJSON.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CAPTURE_SESSION_MAX_S     300
#define CAPTURE_ROOM_LEN          32

void capture_session_init(void);

esp_err_t capture_session_set_room(const char *room);
void capture_session_get_room(char *out, size_t out_len);

bool capture_session_time_ok(void);
bool capture_session_is_active(void);

/** 开始采集。NTP 未同步或已在录则失败。 */
esp_err_t capture_session_start(void);

/** 正常结束（PCM 连接排空后退出）。 */
void capture_session_stop(void);

/** 浏览器断开或出错：丢弃会话。 */
void capture_session_abort(void);

time_t capture_session_started_at(void);
int capture_session_remaining_s(void);

/**
 * 阻塞读取 PCM 字节（int16le）。会话结束后返回 0。
 * @return 字节数，0=会话结束，<0 超时且仍在录
 */
int capture_session_read_pcm(void *buf, size_t buf_len, uint32_t wait_ms);

cJSON *capture_session_status_json(void);
cJSON *capture_session_sample_json(void);

#ifdef __cplusplus
}
#endif
