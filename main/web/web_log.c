#include "web_log.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define WEB_LOG_LINE_MAX 192
#define WEB_LOG_CAPACITY 400

typedef struct {
    uint32_t seq;
    char text[WEB_LOG_LINE_MAX];
} web_log_entry_t;

static web_log_entry_t s_lines[WEB_LOG_CAPACITY];
static uint16_t s_head;
static uint16_t s_count;
static uint32_t s_seq;
static SemaphoreHandle_t s_lock;
static vprintf_like_t s_prev_vprintf;
static bool s_inited;
static bool s_reenter;

static void append_locked(const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return;
    }

    web_log_entry_t *slot = &s_lines[s_head];
    s_seq++;
    slot->seq = s_seq;
    strncpy(slot->text, text, WEB_LOG_LINE_MAX - 1);
    slot->text[WEB_LOG_LINE_MAX - 1] = '\0';

    /* 去掉末尾换行，前端按行渲染 */
    size_t len = strlen(slot->text);
    while (len > 0 && (slot->text[len - 1] == '\n' || slot->text[len - 1] == '\r')) {
        slot->text[--len] = '\0';
    }
    if (len == 0) {
        s_seq--;
        return;
    }

    s_head = (uint16_t)((s_head + 1) % WEB_LOG_CAPACITY);
    if (s_count < WEB_LOG_CAPACITY) {
        s_count++;
    }
}

static int web_log_vprintf(const char *fmt, va_list args)
{
    if (!s_reenter && s_lock != NULL) {
        char buf[WEB_LOG_LINE_MAX];
        va_list copy;
        va_copy(copy, args);
        int n = vsnprintf(buf, sizeof(buf), fmt, copy);
        va_end(copy);
        if (n > 0) {
            s_reenter = true;
            if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
                append_locked(buf);
                xSemaphoreGive(s_lock);
            }
            s_reenter = false;
        }
    }

    if (s_prev_vprintf != NULL) {
        return s_prev_vprintf(fmt, args);
    }
    return vprintf(fmt, args);
}

void web_log_init(void)
{
    if (s_inited) {
        return;
    }
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return;
    }
    s_prev_vprintf = esp_log_set_vprintf(web_log_vprintf);
    s_inited = true;
}

void web_log_clear(void)
{
    if (s_lock == NULL) {
        return;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }
    s_head = 0;
    s_count = 0;
    xSemaphoreGive(s_lock);
}

uint32_t web_log_latest_seq(void)
{
    return s_seq;
}

cJSON *web_log_to_json(uint32_t since_seq, const char *tag_filter, int limit)
{
    if (limit <= 0 || limit > WEB_LOG_CAPACITY) {
        limit = 100;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *lines = cJSON_CreateArray();
    if (root == NULL || lines == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(lines);
        return NULL;
    }

    if (s_lock == NULL || xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        cJSON_AddNumberToObject(root, "seq", s_seq);
        cJSON_AddNumberToObject(root, "count", 0);
        cJSON_AddItemToObject(root, "lines", lines);
        return root;
    }

    /* 从最旧到最新遍历环形缓冲 */
    uint16_t start = (s_count == WEB_LOG_CAPACITY) ? s_head : 0;
    int matched = 0;
    /* since_seq==0：先数出符合过滤的总数，再只取末尾 limit 行 */
    int total_match = 0;
    if (since_seq == 0 && tag_filter != NULL && tag_filter[0] != '\0') {
        for (uint16_t i = 0; i < s_count; i++) {
            const web_log_entry_t *e = &s_lines[(start + i) % WEB_LOG_CAPACITY];
            if (strstr(e->text, tag_filter) != NULL) {
                total_match++;
            }
        }
    } else if (since_seq == 0) {
        total_match = s_count;
    }
    int skip = 0;
    if (since_seq == 0 && total_match > limit) {
        skip = total_match - limit;
    }

    int seen = 0;
    for (uint16_t i = 0; i < s_count; i++) {
        const web_log_entry_t *e = &s_lines[(start + i) % WEB_LOG_CAPACITY];
        if (e->seq <= since_seq) {
            continue;
        }
        if (tag_filter != NULL && tag_filter[0] != '\0' && strstr(e->text, tag_filter) == NULL) {
            continue;
        }
        if (since_seq == 0) {
            if (seen < skip) {
                seen++;
                continue;
            }
        }
        cJSON *item = cJSON_CreateObject();
        if (item == NULL) {
            break;
        }
        cJSON_AddNumberToObject(item, "seq", e->seq);
        cJSON_AddStringToObject(item, "text", e->text);
        cJSON_AddItemToArray(lines, item);
        matched++;
        if (matched >= limit) {
            break;
        }
    }

    cJSON_AddNumberToObject(root, "seq", s_seq);
    cJSON_AddNumberToObject(root, "count", matched);
    cJSON_AddItemToObject(root, "lines", lines);
    xSemaphoreGive(s_lock);
    return root;
}
