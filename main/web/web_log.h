/**
 * @file web_log.h
 * @brief 捕获 ESP_LOG 到环形缓冲，供网页 /api/logs 查看
 */
#pragma once

#include "cJSON.h"
#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 安装 vprintf hook（应尽早调用；串口日志仍会照常输出） */
void web_log_init(void);

/** @brief 清空缓冲 */
void web_log_clear(void);

/** @brief 当前最新序号（单调递增） */
uint32_t web_log_latest_seq(void);

/**
 * @brief 导出日志为 JSON 对象：{seq, count, lines:[{seq,text},...]}
 * @param since_seq 仅返回 seq > since_seq 的行；0 表示按 limit 取最近若干行
 * @param tag_filter 非空时按子串过滤（如 "c4004"）
 * @param limit 最多返回行数（1–256）
 */
cJSON *web_log_to_json(uint32_t since_seq, const char *tag_filter, int limit);

#ifdef __cplusplus
}
#endif
