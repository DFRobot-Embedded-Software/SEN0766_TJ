/**
 * @file yamnet_events.c
 * @brief Multi-event threshold + N-of-M voting over YAMNet-style softmax scores
 *
 * To add an event: append one row to s_event_table (class ids from
 * models/yamnet/yamnet_class_map.csv).
 */

#include "yamnet_events.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

static const char *TAG = "yamnet_evt";

/* AudioSet / YAMNet class ids (yamnet_class_map.csv) */
#define CLS_SPEECH           0
#define CLS_CRY_SOBBING      19
#define CLS_BABY_CRY         20
#define CLS_WHIMPER          21
#define CLS_WAIL_MOAN        22
#define CLS_GLASS            435
#define CLS_SHATTER          437
#define CLS_SMASH_CRASH      463
#define CLS_BREAKING         464
#define CLS_ALARM            382

#ifndef CONFIG_YAMNET_EVENT_VOTE_WINDOW
#define CONFIG_YAMNET_EVENT_VOTE_WINDOW 5
#endif
#ifndef CONFIG_YAMNET_EVENT_VOTE_NEED
#define CONFIG_YAMNET_EVENT_VOTE_NEED 3
#endif
#ifndef CONFIG_YAMNET_EVENT_HOLD_MS
#define CONFIG_YAMNET_EVENT_HOLD_MS 4000
#endif

/**
 * Built-in watch list. Edit here when swapping models / adding events.
 * score_thr_milli: start from model card (~50–220 for soft distill softmax).
 */
static const yamnet_event_desc_t s_event_table[] = {
    {
        .id = "cry",
        .log_name = "CRY",
        /* 婴儿哭声优先：Baby cry + Crying/sobbing，并纳入 Whimper / Wail */
        .class_ids = {CLS_BABY_CRY, CLS_CRY_SOBBING, CLS_WHIMPER, CLS_WAIL_MOAN},
        .class_count = 4,
        /* Soft distill：真哭声常 0.25–0.50；略降阈以便现场上报 */
        .score_thr_milli = 150,
        .vote_window = 5,
        .vote_need = 2,
        .hold_ms = CONFIG_YAMNET_EVENT_HOLD_MS,
        .suppress_if_speech = true,
    },
    {
        .id = "glass",
        .log_name = "GLASS",
        .class_ids = {CLS_SHATTER, CLS_SMASH_CRASH, CLS_BREAKING, CLS_GLASS},
        .class_count = 4,
        .score_thr_milli = 120,
        .vote_window = 3,
        .vote_need = 2,
        .hold_ms = 3000,
        .suppress_if_speech = false,
    },
    {
        .id = "alarm",
        .log_name = "ALARM",
        .class_ids = {CLS_ALARM, -1, -1, -1},
        .class_count = 1,
        .score_thr_milli = 150,
        .vote_window = CONFIG_YAMNET_EVENT_VOTE_WINDOW,
        .vote_need = CONFIG_YAMNET_EVENT_VOTE_NEED,
        .hold_ms = CONFIG_YAMNET_EVENT_HOLD_MS,
        .suppress_if_speech = false,
    },
};

#define EVENT_N (sizeof(s_event_table) / sizeof(s_event_table[0]))

typedef struct {
    uint8_t votes[YAMNET_EVENT_VOTE_CAP];
    uint8_t pos;
    uint8_t filled;
    bool active;
    int64_t last_alert_us;
} event_runtime_t;

static event_runtime_t s_rt[YAMNET_EVENT_MAX];
static char s_last_event_id[24];
static char s_last_event_label[32];
static int64_t s_last_event_us;

static float class_at(const float *probs, int n_classes, int class_id)
{
    if (probs == NULL || class_id < 0 || class_id >= n_classes) {
        return 0.0f;
    }
    return probs[class_id];
}

static float event_score(const yamnet_event_desc_t *ev, const float *probs, int n_classes)
{
    float s = 0.0f;
    for (uint8_t i = 0; i < ev->class_count && i < YAMNET_EVENT_MAX_CLASSES; i++) {
        int id = ev->class_ids[i];
        if (id < 0) {
            break;
        }
        s += class_at(probs, n_classes, id);
    }
    return s;
}

static bool frame_hit(const yamnet_event_desc_t *ev, float score, float speech)
{
    const float thr = (float)ev->score_thr_milli / 1000.0f;
    if (score < thr) {
        return false;
    }
    if (ev->suppress_if_speech) {
        /* Only drop when Speech clearly wins (talk), not when co-present with cry */
        if (speech > score && speech >= 0.18f) {
            return false;
        }
    }
    return true;
}

static int vote_push(event_runtime_t *rt, const yamnet_event_desc_t *ev, bool hit)
{
    int win = ev->vote_window;
    if (win < 1) {
        win = 1;
    }
    if (win > YAMNET_EVENT_VOTE_CAP) {
        win = YAMNET_EVENT_VOTE_CAP;
    }
    rt->votes[rt->pos] = hit ? 1 : 0;
    rt->pos = (uint8_t)((rt->pos + 1) % (uint8_t)win);
    if (rt->filled < (uint8_t)win) {
        rt->filled++;
    }
    int sum = 0;
    for (uint8_t i = 0; i < rt->filled; i++) {
        sum += rt->votes[i];
    }
    return sum;
}

void yamnet_events_init(void)
{
    memset(s_rt, 0, sizeof(s_rt));
    ESP_LOGI(TAG, "event table: %u watchers", (unsigned)EVENT_N);
    for (size_t i = 0; i < EVENT_N; i++) {
        const yamnet_event_desc_t *e = &s_event_table[i];
        ESP_LOGI(TAG, "  [%s] thr=%.3f vote=%u/%u hold=%ums speech_sup=%d",
                 e->id,
                 (double)e->score_thr_milli / 1000.0,
                 (unsigned)e->vote_need,
                 (unsigned)e->vote_window,
                 (unsigned)e->hold_ms,
                 (int)e->suppress_if_speech);
    }
}

void yamnet_events_on_probs(const float *probs,
                            int n_classes,
                            float speech_score,
                            yamnet_event_alert_cb_t alert_cb,
                            void *user_ctx)
{
    if (probs == NULL || n_classes <= 0) {
        return;
    }

    const int64_t now_us = esp_timer_get_time();
    const size_t n = (EVENT_N < YAMNET_EVENT_MAX) ? EVENT_N : YAMNET_EVENT_MAX;

    for (size_t i = 0; i < n; i++) {
        const yamnet_event_desc_t *ev = &s_event_table[i];
        event_runtime_t *rt = &s_rt[i];
        const float score = event_score(ev, probs, n_classes);
        const bool hit = frame_hit(ev, score, speech_score);
        const int votes = vote_push(rt, ev, hit);
        const int need = ev->vote_need > 0 ? ev->vote_need : 1;
        const bool alert_ready = (votes >= need) && (rt->filled >= (uint8_t)need);
        const int64_t hold_us = (int64_t)ev->hold_ms * 1000;

        if (hit) {
            ESP_LOGI(TAG, "%s_HIT score=%.3f votes=%d/%u speech=%.3f",
                     ev->log_name, score, votes, (unsigned)ev->vote_window, speech_score);
        }

        if (alert_ready) {
            if (!rt->active || (now_us - rt->last_alert_us) >= hold_us) {
                rt->active = true;
                rt->last_alert_us = now_us;
                ESP_LOGW(TAG, "%s_ALERT score=%.3f votes=%d/%u",
                         ev->log_name, score, votes, (unsigned)ev->vote_window);
                strncpy(s_last_event_id, ev->id, sizeof(s_last_event_id) - 1);
                s_last_event_id[sizeof(s_last_event_id) - 1] = '\0';
                /* 中文展示名供 Web 声音事件 */
                if (strcmp(ev->id, "cry") == 0) {
                    strncpy(s_last_event_label, "婴儿哭泣", sizeof(s_last_event_label) - 1);
                } else if (strcmp(ev->id, "glass") == 0) {
                    strncpy(s_last_event_label, "检测到玻璃碎裂", sizeof(s_last_event_label) - 1);
                } else if (strcmp(ev->id, "alarm") == 0) {
                    strncpy(s_last_event_label, "检测到警报声", sizeof(s_last_event_label) - 1);
                } else {
                    strncpy(s_last_event_label, ev->log_name, sizeof(s_last_event_label) - 1);
                }
                s_last_event_label[sizeof(s_last_event_label) - 1] = '\0';
                s_last_event_us = now_us;
                if (alert_cb) {
                    alert_cb(ev, score, votes, user_ctx);
                }
            } else if (strcmp(ev->id, "cry") == 0) {
                /* 持续哭声：刷新时间戳，Web 声音事件保持「刚刚」 */
                strncpy(s_last_event_id, "cry", sizeof(s_last_event_id) - 1);
                s_last_event_id[sizeof(s_last_event_id) - 1] = '\0';
                strncpy(s_last_event_label, "婴儿哭泣", sizeof(s_last_event_label) - 1);
                s_last_event_label[sizeof(s_last_event_label) - 1] = '\0';
                s_last_event_us = now_us;
            }
        } else if (rt->active && votes == 0) {
            rt->active = false;
            ESP_LOGI(TAG, "%s_CLEAR", ev->log_name);
        }
    }
}

float yamnet_events_score(const char *event_id, const float *probs, int n_classes)
{
    if (event_id == NULL || probs == NULL) {
        return 0.0f;
    }
    for (size_t i = 0; i < EVENT_N; i++) {
        if (strcmp(s_event_table[i].id, event_id) == 0) {
            return event_score(&s_event_table[i], probs, n_classes);
        }
    }
    return 0.0f;
}

int yamnet_events_count(void)
{
    return (int)EVENT_N;
}

const yamnet_event_desc_t *yamnet_events_get(int index)
{
    if (index < 0 || (size_t)index >= EVENT_N) {
        return NULL;
    }
    return &s_event_table[index];
}

bool yamnet_events_get_last(char *type_out, size_t type_len, char *label_out, size_t label_len, uint32_t *age_ms)
{
    if (s_last_event_us == 0 || s_last_event_id[0] == '\0') {
        return false;
    }
    if (type_out != NULL && type_len > 0) {
        strncpy(type_out, s_last_event_id, type_len - 1);
        type_out[type_len - 1] = '\0';
    }
    if (label_out != NULL && label_len > 0) {
        strncpy(label_out, s_last_event_label, label_len - 1);
        label_out[label_len - 1] = '\0';
    }
    if (age_ms != NULL) {
        int64_t age = (esp_timer_get_time() - s_last_event_us) / 1000;
        *age_ms = age > 0 ? (uint32_t)age : 0;
    }
    return true;
}
