#include "c4004_sensor.h"

#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "c4004";

#define C4004_FRAME_HEAD1       0x53
#define C4004_FRAME_HEAD2       0x59
#define C4004_FRAME_TAIL1       0x54
#define C4004_FRAME_TAIL2       0x43
#define C4004_QUERY_DATA         0x0F
#define C4004_MAX_PAYLOAD        612
#define C4004_TAG_RECORD_LEN     12
#define C4004_SYNC_TIMEOUT_BASE_MS 500
#define C4004_SYNC_TIMEOUT_PER_TAG_MS 250
#define C4004_DEFAULT_TIMEOUT_MS C4004_SYNC_TIMEOUT_BASE_MS
#define C4004_UART_BUF_SIZE      1024

#define C4004_CTRL_SYSTEM          0x01
#define C4004_CTRL_WORK_STATUS     0x05
#define C4004_CTRL_DETECTION_RANGE 0x07

#define C4004_CMD_HEARTBEAT_QUERY       0x80
#define C4004_CMD_INIT_FINISHED_QUERY   0x81
#define C4004_CMD_INIT_FINISHED_REPORT  0x01
#define C4004_CMD_SET_TAGS_FROM_CONFIG  0x19
#define C4004_CMD_TAG_REPORT            0x1B
#define C4004_CMD_CLEAR_TAG             0x13
#define C4004_CMD_SET_TAG               0x11
#define C4004_CMD_SET_RANGE             0x1A

#define C4004_RANGE_FOUR_SIDE           0x04

#define C4004_CTRL_TRAJECTORY              0x82
#define C4004_CMD_TRAJECTORY_SET_ENABLE    0x00
#define C4004_CMD_TRAJECTORY_TARGET_REPORT 0x02
#define C4004_CMD_TRAJECTORY_QUERY_TARGET  0x82
#define C4004_CMD_TRAJECTORY_SET_CHECK_TO_ACTIVE 0x0D

#define C4004_TARGET_RECORD_LEN 11
#define C4004_MAX_TARGETS       8
#define C4004_SIGNAL_HOLD_MS    2500

#define C4004_TAG_TYPE_BOUNDARY        0x01
#define C4004_TAG_TYPE_APPROACH_AWAY   0x02
#define C4004_TAG_TYPE_PEOPLE_COUNT    0x03
#define C4004_TAG_SCOPE_RECT           0x01

typedef struct {
    uint8_t control;
    uint8_t cmd;
    uint16_t len;
    uint8_t data[C4004_MAX_PAYLOAD];
} c4004_packet_t;

typedef struct {
    bool valid;
    uint8_t strategy;
    int active;
    int static_count;
    bool enter;
    bool exit;
    bool approaching;
    bool leaving;
} c4004_tag_state_t;

typedef struct {
    uint8_t index;
    uint8_t kinesia;
    int16_t x;
    int16_t y;
    int16_t speed;
} c4004_target_t;

static SemaphoreHandle_t s_uart_lock;
static SemaphoreHandle_t s_state_lock;
static bool s_ready;
static c4004_tag_state_t s_tag_states[AREA_TAG_MAX];
static c4004_target_t s_targets[C4004_MAX_TARGETS];
static uint8_t s_target_count;
static TickType_t s_approach_hold_until[AREA_TAG_MAX];
static TickType_t s_leave_hold_until[AREA_TAG_MAX];
static TickType_t s_enter_hold_until[AREA_TAG_MAX];
static TickType_t s_exit_hold_until[AREA_TAG_MAX];

/** UART 请求共用缓冲区，避免 sync/query 在 main 任务栈上分配 ~1.2KB */
static c4004_packet_t s_work_packet;
static uint8_t s_tx_payload[C4004_MAX_PAYLOAD];

static uint16_t read_u16(const uint8_t *data)
{
    return ((uint16_t)data[0] << 8) | data[1];
}

static int16_t read_sign_i16(const uint8_t *data)
{
    uint16_t raw = read_u16(data);
    int16_t magnitude = (int16_t)(raw & 0x7FFF);
    return (raw & 0x8000) ? (int16_t)(-magnitude) : magnitude;
}

static void write_u16(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)(value >> 8);
    data[1] = (uint8_t)(value & 0xFF);
}

static void write_sign_i16(uint8_t *data, int16_t value)
{
    int32_t magnitude = value;
    uint16_t raw = 0;
    if (magnitude < 0) {
        magnitude = -magnitude;
        raw = 0x8000;
    }
    raw |= ((uint16_t)magnitude & 0x7FFF);
    write_u16(data, raw);
}

static void uart_flush_rx(void)
{
    uart_flush_input(CONFIG_C4004_UART_PORT);
}

static bool uart_read_byte(uint8_t *value, uint32_t timeout_ms)
{
    int n = uart_read_bytes(CONFIG_C4004_UART_PORT, value, 1, pdMS_TO_TICKS(timeout_ms));
    return n == 1;
}

static bool send_command(uint8_t control, uint8_t cmd, const uint8_t *data, uint16_t len)
{
    uint8_t checksum = 0;
    uint8_t byte = C4004_FRAME_HEAD1;
    checksum += byte;
    if (uart_write_bytes(CONFIG_C4004_UART_PORT, &byte, 1) != 1) {
        return false;
    }
    byte = C4004_FRAME_HEAD2;
    checksum += byte;
    if (uart_write_bytes(CONFIG_C4004_UART_PORT, &byte, 1) != 1) {
        return false;
    }
    checksum += control;
    if (uart_write_bytes(CONFIG_C4004_UART_PORT, &control, 1) != 1) {
        return false;
    }
    checksum += cmd;
    if (uart_write_bytes(CONFIG_C4004_UART_PORT, &cmd, 1) != 1) {
        return false;
    }
    byte = (uint8_t)(len >> 8);
    checksum += byte;
    if (uart_write_bytes(CONFIG_C4004_UART_PORT, &byte, 1) != 1) {
        return false;
    }
    byte = (uint8_t)(len & 0xFF);
    checksum += byte;
    if (uart_write_bytes(CONFIG_C4004_UART_PORT, &byte, 1) != 1) {
        return false;
    }
    for (uint16_t i = 0; i < len; i++) {
        byte = data ? data[i] : 0;
        checksum += byte;
        if (uart_write_bytes(CONFIG_C4004_UART_PORT, &byte, 1) != 1) {
            return false;
        }
    }
    if (uart_write_bytes(CONFIG_C4004_UART_PORT, &checksum, 1) != 1) {
        return false;
    }
    byte = C4004_FRAME_TAIL1;
    if (uart_write_bytes(CONFIG_C4004_UART_PORT, &byte, 1) != 1) {
        return false;
    }
    byte = C4004_FRAME_TAIL2;
    return uart_write_bytes(CONFIG_C4004_UART_PORT, &byte, 1) == 1;
}

static bool read_frame(c4004_packet_t *packet, uint32_t timeout_ms)
{
    uint32_t start = xTaskGetTickCount();
    uint8_t value = 0;
    uint8_t checksum = 0;
    uint8_t recv_checksum = 0;

    if (packet == NULL) {
        return false;
    }
    memset(packet, 0, sizeof(*packet));

    while ((xTaskGetTickCount() - start) < pdMS_TO_TICKS(timeout_ms)) {
        if (!uart_read_byte(&value, 20)) {
            continue;
        }
        if (value != C4004_FRAME_HEAD1) {
            continue;
        }
        checksum = value;
        if (!uart_read_byte(&value, timeout_ms) || value != C4004_FRAME_HEAD2) {
            continue;
        }
        checksum += value;
        if (!uart_read_byte(&packet->control, timeout_ms)) {
            return false;
        }
        checksum += packet->control;
        if (!uart_read_byte(&packet->cmd, timeout_ms)) {
            return false;
        }
        checksum += packet->cmd;
        if (!uart_read_byte(&value, timeout_ms)) {
            return false;
        }
        checksum += value;
        packet->len = ((uint16_t)value << 8);
        if (!uart_read_byte(&value, timeout_ms)) {
            return false;
        }
        checksum += value;
        packet->len |= value;
        if (packet->len > C4004_MAX_PAYLOAD) {
            uart_flush_rx();
            return false;
        }
        for (uint16_t i = 0; i < packet->len; i++) {
            if (!uart_read_byte(&packet->data[i], timeout_ms)) {
                return false;
            }
            checksum += packet->data[i];
        }
        if (!uart_read_byte(&recv_checksum, timeout_ms)) {
            return false;
        }
        if (!uart_read_byte(&value, timeout_ms) || value != C4004_FRAME_TAIL1) {
            return false;
        }
        if (!uart_read_byte(&value, timeout_ms) || value != C4004_FRAME_TAIL2) {
            return false;
        }
        return checksum == recv_checksum;
    }
    return false;
}

static bool request_frame(uint8_t control, uint8_t cmd, const uint8_t *data, uint16_t len,
                          c4004_packet_t *response, uint32_t timeout_ms)
{
    uint32_t start = xTaskGetTickCount();

    if (response == NULL) {
        return false;
    }
    xSemaphoreTake(s_uart_lock, portMAX_DELAY);
    uart_flush_rx();
    if (!send_command(control, cmd, data, len)) {
        xSemaphoreGive(s_uart_lock);
        return false;
    }
    while ((xTaskGetTickCount() - start) < pdMS_TO_TICKS(timeout_ms)) {
        uint32_t elapsed = (xTaskGetTickCount() - start) * portTICK_PERIOD_MS;
        uint32_t left = (elapsed >= timeout_ms) ? 1 : (timeout_ms - elapsed);
        if (read_frame(response, left)) {
            if (response->control == control && response->cmd == cmd) {
                xSemaphoreGive(s_uart_lock);
                return true;
            }
        }
    }
    xSemaphoreGive(s_uart_lock);
    return false;
}

static bool wait_init_finished(void)
{
    uint8_t query = C4004_QUERY_DATA;
    c4004_packet_t packet;
    uint32_t start = xTaskGetTickCount();

    while ((xTaskGetTickCount() - start) < pdMS_TO_TICKS(5000)) {
        if (request_frame(C4004_CTRL_WORK_STATUS, C4004_CMD_INIT_FINISHED_QUERY,
                          &query, 1, &packet, C4004_DEFAULT_TIMEOUT_MS)) {
            if (packet.len > 0 && packet.data[0] == 0x01) {
                return true;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return false;
}

static bool check_heartbeat(void)
{
    uint8_t query = C4004_QUERY_DATA;
    c4004_packet_t packet;
    return request_frame(C4004_CTRL_SYSTEM, C4004_CMD_HEARTBEAT_QUERY,
                           &query, 1, &packet, C4004_DEFAULT_TIMEOUT_MS);
}

static uint8_t web_strategy_to_tag_type(uint8_t strategy)
{
    switch (strategy) {
    case 2:
        return C4004_TAG_TYPE_BOUNDARY;
    case 3:
        return C4004_TAG_TYPE_APPROACH_AWAY;
    default:
        return C4004_TAG_TYPE_PEOPLE_COUNT;
    }
}

static void parse_targets(const uint8_t *data, uint16_t len)
{
    if (data == NULL || len < C4004_TARGET_RECORD_LEN) {
        s_target_count = 0;
        return;
    }

    uint8_t count = (uint8_t)(len / C4004_TARGET_RECORD_LEN);
    if (count > C4004_MAX_TARGETS) {
        count = C4004_MAX_TARGETS;
    }
    s_target_count = count;

    for (uint8_t i = 0; i < count; i++) {
        uint16_t off = (uint16_t)(i * C4004_TARGET_RECORD_LEN);
        s_targets[i].index = data[off];
        s_targets[i].kinesia = data[off + 1];
        s_targets[i].x = read_sign_i16(&data[off + 3]);
        s_targets[i].y = read_sign_i16(&data[off + 5]);
        s_targets[i].speed = read_sign_i16(&data[off + 9]);
    }

    ESP_LOGD(TAG, "targets updated: count=%u", count);
    for (uint8_t i = 0; i < count; i++) {
        ESP_LOGD(TAG, "  target[%u] idx=%u kin=0x%02X x=%d y=%d spd=%d cm",
                 i, s_targets[i].index, s_targets[i].kinesia,
                 (int)s_targets[i].x, (int)s_targets[i].y, (int)s_targets[i].speed);
    }
}

/** 角点标定：多目标时优先选运动目标，避免始终返回 targets[0] */
static int pick_coord_target_index(void)
{
    if (s_target_count == 0) {
        return -1;
    }
    if (s_target_count == 1) {
        return 0;
    }

    int motion_idx = -1;
    int best_speed_idx = 0;
    int16_t best_speed = 0;

    for (uint8_t i = 0; i < s_target_count; i++) {
        int16_t spd = s_targets[i].speed;
        if (spd < 0) {
            spd = (int16_t)(-spd);
        }
        if (spd > best_speed) {
            best_speed = spd;
            best_speed_idx = (int)i;
        }
        if (s_targets[i].kinesia == 0x02) {
            motion_idx = (int)i;
        }
    }

    if (motion_idx >= 0) {
        return motion_idx;
    }
    return best_speed_idx;
}

static bool enable_trajectory_track(void)
{
    uint8_t enable = 1;
    c4004_packet_t packet;
    return request_frame(C4004_CTRL_TRAJECTORY, C4004_CMD_TRAJECTORY_SET_ENABLE,
                         &enable, 1, &packet, C4004_DEFAULT_TIMEOUT_MS);
}

static bool set_check_to_active_frames(uint8_t frames)
{
    if (frames < 1 || frames > 7) {
        frames = 7;
    }
    c4004_packet_t packet;
    if (!request_frame(C4004_CTRL_TRAJECTORY, C4004_CMD_TRAJECTORY_SET_CHECK_TO_ACTIVE,
                       &frames, 1, &packet, C4004_DEFAULT_TIMEOUT_MS)) {
        ESP_LOGW(TAG, "set check-to-active frames timeout");
        return false;
    }
    ESP_LOGI(TAG, "check-to-active frames set to %u", frames);
    return true;
}

static const char *strategy_name(uint8_t strategy)
{
    switch (strategy) {
    case 2:
        return "边缘门";
    case 3:
        return "靠近远离";
    default:
        return "统计";
    }
}

static const char *tag_type_name(uint8_t tag_type)
{
    switch (tag_type) {
    case C4004_TAG_TYPE_BOUNDARY:
        return "boundary";
    case C4004_TAG_TYPE_APPROACH_AWAY:
        return "approach_away";
    default:
        return "people_count";
    }
}

static void log_tag_event_raw(const uint8_t *data, uint16_t len)
{
    if (data == NULL || len == 0) {
        return;
    }
    char hex[64] = {0};
    uint16_t n = len < 16 ? len : 16;
    size_t off = 0;
    for (uint16_t i = 0; i < n && off + 3 < sizeof(hex); i++) {
        off += (size_t)snprintf(hex + off, sizeof(hex) - off, "%02X ", data[i]);
    }
    ESP_LOGI(TAG, "tag frame (%u bytes): %s", len, hex);
}

static void hold_signal(TickType_t *slot)
{
    *slot = xTaskGetTickCount() + pdMS_TO_TICKS(C4004_SIGNAL_HOLD_MS);
}

static bool signal_active(TickType_t until)
{
    return until != 0 && xTaskGetTickCount() < until;
}

static void apply_boundary_value(c4004_tag_state_t *st, uint8_t tag_index, uint8_t value)
{
    st->enter = false;
    st->exit = false;
    /* 与官方 readTagEvent 示例一致：0=进入，非 0=离开 */
    if (value == 0x00) {
        st->enter = true;
        hold_signal(&s_enter_hold_until[tag_index]);
    } else {
        st->exit = true;
        hold_signal(&s_exit_hold_until[tag_index]);
    }
}

static void apply_motion_dir(c4004_tag_state_t *st, uint8_t tag_index, uint8_t value)
{
    st->approaching = false;
    st->leaving = false;
    switch (value) {
    case 0x01:
        st->approaching = true;
        hold_signal(&s_approach_hold_until[tag_index]);
        break;
    case 0x02:
        st->leaving = true;
        hold_signal(&s_leave_hold_until[tag_index]);
        break;
    case 0x00:
        /* 部分固件用 0 表示远离/在远区 */
        st->leaving = true;
        hold_signal(&s_leave_hold_until[tag_index]);
        break;
    default:
        if (value & 0x01) {
            st->approaching = true;
            hold_signal(&s_approach_hold_until[tag_index]);
        }
        if (value & 0x02) {
            st->leaving = true;
            hold_signal(&s_leave_hold_until[tag_index]);
        }
        if (!st->approaching && !st->leaving) {
            ESP_LOGW(TAG, "unknown motionDir=0x%02X", value);
        }
        break;
    }
}

static esp_err_t clear_all_tags_locked(void)
{
    uint8_t clear_all = 0xFF;
    if (!request_frame(C4004_CTRL_DETECTION_RANGE, C4004_CMD_CLEAR_TAG,
                       &clear_all, 1, &s_work_packet, C4004_DEFAULT_TIMEOUT_MS)) {
        ESP_LOGW(TAG, "clear all tags timeout");
        return ESP_ERR_TIMEOUT;
    }
    if (s_work_packet.len > 0 && s_work_packet.data[0] == 0xFE) {
        ESP_LOGW(TAG, "clear all tags rejected (0xFE)");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "C4004 all tags cleared");
    return ESP_OK;
}

static esp_err_t clear_tag_index(uint8_t index)
{
    uint8_t data[2];
    write_u16(data, index);
    if (!request_frame(C4004_CTRL_DETECTION_RANGE, C4004_CMD_CLEAR_TAG,
                       data, sizeof(data), &s_work_packet, C4004_DEFAULT_TIMEOUT_MS)) {
        ESP_LOGW(TAG, "clear tag[%u] timeout", index);
        return ESP_ERR_TIMEOUT;
    }
    if (s_work_packet.len >= 2 && read_u16(s_work_packet.data) != index) {
        ESP_LOGW(TAG, "clear tag[%u] ack index mismatch", index);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "C4004 tag[%u] cleared", index);
    return ESP_OK;
}

static void clear_orphan_tags(uint8_t new_count, uint8_t prev_count)
{
    for (uint8_t i = new_count; i < prev_count; i++) {
        esp_err_t err = clear_tag_index(i);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "clear orphan tag[%u] failed: %s", i, esp_err_to_name(err));
        }
    }
}

static void handle_packet(const c4004_packet_t *packet);

static void append_tag_to_payload(uint16_t *offset, uint8_t index, const area_tag_t *tag,
                                  const area_rect_t *rect)
{
    uint8_t tag_type = web_strategy_to_tag_type(tag->strategy);
    s_tx_payload[(*offset)++] = index;
    s_tx_payload[(*offset)++] = tag_type;
    s_tx_payload[(*offset)++] = C4004_TAG_SCOPE_RECT;
    s_tx_payload[(*offset)++] = 0;
    write_sign_i16(&s_tx_payload[*offset], rect->center_x);
    *offset += 2;
    write_sign_i16(&s_tx_payload[*offset], rect->center_y);
    *offset += 2;
    write_u16(&s_tx_payload[*offset], rect->width);
    *offset += 2;
    write_u16(&s_tx_payload[*offset], rect->height);
    *offset += 2;
}

static esp_err_t build_tags_payload(const area_config_t *cfg, uint16_t *out_len)
{
    if (cfg == NULL || out_len == NULL || cfg->tag_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t offset = 0;
    write_u16(&s_tx_payload[offset], cfg->tag_count);
    offset += 2;

    for (uint8_t i = 0; i < cfg->tag_count; i++) {
        const area_tag_t *tag = &cfg->tags[i];
        area_rect_t rect = {0};
        esp_err_t rect_err = area_coords_to_rect_for_radar(tag, &rect);
        if (rect_err != ESP_OK) {
            ESP_LOGE(TAG, "tag[%u] \"%s\" rect invalid: %s",
                     i, tag->name, esp_err_to_name(rect_err));
            return rect_err;
        }

        append_tag_to_payload(&offset, i, tag, &rect);

        ESP_LOGI(TAG,
                 "tag[%u] \"%s\" strategy=%s(%u) -> center=(%d,%d) size=%ux%u cm type=%s",
                 i, tag->name, strategy_name(tag->strategy), tag->strategy,
                 (int)rect.center_x, (int)rect.center_y,
                 (unsigned)rect.width, (unsigned)rect.height,
                 tag_type_name(web_strategy_to_tag_type(tag->strategy)));
    }

    *out_len = offset;
    return ESP_OK;
}

static uint32_t sync_timeout_ms(uint8_t tag_count)
{
    return C4004_SYNC_TIMEOUT_BASE_MS + (uint32_t)tag_count * C4004_SYNC_TIMEOUT_PER_TAG_MS;
}

static esp_err_t send_tags_from_config(uint16_t payload_len, uint8_t tag_count)
{
    const uint16_t expected_len = (uint16_t)(2 + (uint16_t)tag_count * C4004_TAG_RECORD_LEN);
    const uint32_t timeout_ms = sync_timeout_ms(tag_count);

    xSemaphoreTake(s_uart_lock, portMAX_DELAY);
    uart_flush_rx();
    if (!send_command(C4004_CTRL_DETECTION_RANGE, C4004_CMD_SET_TAGS_FROM_CONFIG,
                      s_tx_payload, payload_len)) {
        xSemaphoreGive(s_uart_lock);
        return ESP_FAIL;
    }

    uint32_t start = xTaskGetTickCount();
    bool ok = false;
    while ((xTaskGetTickCount() - start) < pdMS_TO_TICKS(timeout_ms)) {
        if (read_frame(&s_work_packet, 50)) {
            handle_packet(&s_work_packet);
            if (s_work_packet.control == C4004_CTRL_DETECTION_RANGE &&
                s_work_packet.cmd == C4004_CMD_SET_TAGS_FROM_CONFIG) {
                if (s_work_packet.len != expected_len) {
                    ESP_LOGW(TAG, "set tags ack len %u != %u", s_work_packet.len, expected_len);
                } else if (read_u16(s_work_packet.data) != tag_count) {
                    ESP_LOGW(TAG, "set tags ack count %u != %u",
                             read_u16(s_work_packet.data), tag_count);
                } else {
                    ok = true;
                }
                break;
            }
        }
    }
    xSemaphoreGive(s_uart_lock);
    return ok ? ESP_OK : ESP_ERR_TIMEOUT;
}

static esp_err_t sync_detection_range(const area_config_t *cfg)
{
    if (cfg == NULL || cfg->tag_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    float min_x = cfg->tags[0].coords[0][0];
    float max_x = min_x;
    float min_y = cfg->tags[0].coords[0][1];
    float max_y = min_y;

    for (uint8_t t = 0; t < cfg->tag_count; t++) {
        for (int p = 0; p < 4; p++) {
            const float x = cfg->tags[t].coords[p][0];
            const float y = cfg->tags[t].coords[p][1];
            if (x < min_x) {
                min_x = x;
            }
            if (x > max_x) {
                max_x = x;
            }
            if (y < min_y) {
                min_y = y;
            }
            if (y > max_y) {
                max_y = y;
            }
        }
    }

    const float margin = 80.0f;
    min_x -= margin;
    max_x += margin;
    min_y -= margin;
    max_y += margin;

    uint8_t data[9];
    data[0] = C4004_RANGE_FOUR_SIDE;
    write_sign_i16(&data[1], (int16_t)max_x);
    write_sign_i16(&data[3], (int16_t)min_x);
    write_sign_i16(&data[5], (int16_t)max_y);
    write_sign_i16(&data[7], (int16_t)min_y);

    if (!request_frame(C4004_CTRL_DETECTION_RANGE, C4004_CMD_SET_RANGE,
                       data, sizeof(data), &s_work_packet, C4004_DEFAULT_TIMEOUT_MS)) {
        ESP_LOGW(TAG, "set detection range timeout");
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "detection range x=[%.0f,%.0f] y=[%.0f,%.0f] cm",
             min_x, max_x, min_y, max_y);
    return ESP_OK;
}

static uint8_t tag_type_to_strategy(uint8_t tag_type)
{
    switch (tag_type) {
    case C4004_TAG_TYPE_BOUNDARY:
        return 2;
    case C4004_TAG_TYPE_APPROACH_AWAY:
        return 3;
    default:
        return 1;
    }
}

static void parse_tag_event(const uint8_t *data, uint16_t len)
{
    if (data == NULL || len < 8) {
        ESP_LOGW(TAG, "tag event too short (len=%u)", len);
        return;
    }

    log_tag_event_raw(data, len);

    uint8_t tag_index = data[0];
    uint8_t tag_type = data[1];
    uint8_t value = data[7];

    if (tag_index >= AREA_TAG_MAX) {
        ESP_LOGW(TAG, "tag index out of range: %u", tag_index);
        return;
    }

    c4004_tag_state_t *st = &s_tag_states[tag_index];
    st->valid = true;
    st->strategy = tag_type_to_strategy(tag_type);

    if (tag_type == C4004_TAG_TYPE_BOUNDARY) {
        apply_boundary_value(st, tag_index, value);
        st->approaching = false;
        st->leaving = false;
        st->active = 0;
        st->static_count = 0;
        ESP_LOGI(TAG, "boundary tag[%u] enterExit=0x%02X enter=%d exit=%d",
                 tag_index, value, st->enter, st->exit);
    } else if (tag_type == C4004_TAG_TYPE_APPROACH_AWAY) {
        apply_motion_dir(st, tag_index, value);
        st->enter = false;
        st->exit = false;
        st->active = 0;
        st->static_count = 0;
        ESP_LOGI(TAG, "approach_away tag[%u] motionDir=0x%02X approaching=%d leaving=%d",
                 tag_index, value, st->approaching, st->leaving);
    } else if (tag_type == C4004_TAG_TYPE_PEOPLE_COUNT) {
        st->active = (int)((value >> 4) & 0x0F);
        st->static_count = (int)(value & 0x0F);
        st->enter = false;
        st->exit = false;
        st->approaching = false;
        st->leaving = false;
        ESP_LOGI(TAG, "people_count tag[%u] motion=%d static=%d (raw=0x%02X)",
                 tag_index, st->active, st->static_count, value);
    } else {
        ESP_LOGW(TAG, "unknown tag type %s(%u) idx=%u value=0x%02X",
                 tag_type_name(tag_type), tag_type, tag_index, value);
    }
}

static void handle_packet(const c4004_packet_t *packet)
{
    if (packet == NULL) {
        return;
    }
    if (packet->control == C4004_CTRL_TRAJECTORY &&
        (packet->cmd == C4004_CMD_TRAJECTORY_TARGET_REPORT ||
         packet->cmd == C4004_CMD_TRAJECTORY_QUERY_TARGET)) {
        xSemaphoreTake(s_state_lock, portMAX_DELAY);
        parse_targets(packet->data, packet->len);
        xSemaphoreGive(s_state_lock);
        return;
    }
    if (packet->control == C4004_CTRL_DETECTION_RANGE && packet->cmd == C4004_CMD_TAG_REPORT) {
        xSemaphoreTake(s_state_lock, portMAX_DELAY);
        parse_tag_event(packet->data, packet->len);
        xSemaphoreGive(s_state_lock);
        return;
    }
    if (packet->control == C4004_CTRL_DETECTION_RANGE) {
        ESP_LOGD(TAG, "detection_range cmd=0x%02X len=%u", packet->cmd, packet->len);
    }
}

static void c4004_poll_task(void *arg)
{
    (void)arg;
    c4004_packet_t packet;

    while (1) {
        xSemaphoreTake(s_uart_lock, portMAX_DELAY);
        bool got = read_frame(&packet, 50);
        xSemaphoreGive(s_uart_lock);
        if (got) {
            handle_packet(&packet);
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

static esp_err_t init_uart(void)
{
    uart_config_t cfg = {
        .baud_rate = CONFIG_C4004_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_driver_install(CONFIG_C4004_UART_PORT, C4004_UART_BUF_SIZE * 2,
                                        C4004_UART_BUF_SIZE, 0, NULL, 0);
    if (err != ESP_OK) {
        return err;
    }
    err = uart_param_config(CONFIG_C4004_UART_PORT, &cfg);
    if (err != ESP_OK) {
        return err;
    }
    return uart_set_pin(CONFIG_C4004_UART_PORT, CONFIG_C4004_UART_TX, CONFIG_C4004_UART_RX,
                          UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
}

esp_err_t c4004_sensor_start(void)
{
    s_uart_lock = xSemaphoreCreateMutex();
    s_state_lock = xSemaphoreCreateMutex();
    if (s_uart_lock == NULL || s_state_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = init_uart();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART init failed: %s", esp_err_to_name(err));
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(100));

    if (!wait_init_finished() && !check_heartbeat()) {
        ESP_LOGE(TAG, "C4004 not responding (TX=%d RX=%d)", CONFIG_C4004_UART_TX, CONFIG_C4004_UART_RX);
        return ESP_ERR_NOT_FOUND;
    }

    if (!enable_trajectory_track()) {
        ESP_LOGW(TAG, "trajectory track enable failed (coord capture may not work)");
    } else {
        ESP_LOGI(TAG, "trajectory track enabled");
    }

    if (!set_check_to_active_frames(7)) {
        ESP_LOGW(TAG, "check-to-active frames setup failed");
    }

    s_ready = true;
    ESP_LOGI(TAG, "C4004 ready (UART%d TX=%d RX=%d %dbps)",
             CONFIG_C4004_UART_PORT, CONFIG_C4004_UART_TX, CONFIG_C4004_UART_RX, CONFIG_C4004_UART_BAUD);

    if (xTaskCreate(c4004_poll_task, "c4004", 4096, NULL, 5, NULL) != pdPASS) {
        s_ready = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t c4004_sensor_sync_tags(const area_config_t *cfg, uint8_t prev_count)
{
    if (!s_ready || cfg == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (cfg->tag_count == 0) {
        esp_err_t err = clear_all_tags_locked();
        xSemaphoreTake(s_state_lock, portMAX_DELAY);
        memset(s_tag_states, 0, sizeof(s_tag_states));
        memset(s_approach_hold_until, 0, sizeof(s_approach_hold_until));
        memset(s_leave_hold_until, 0, sizeof(s_leave_hold_until));
        memset(s_enter_hold_until, 0, sizeof(s_enter_hold_until));
        memset(s_exit_hold_until, 0, sizeof(s_exit_hold_until));
        xSemaphoreGive(s_state_lock);
        return err;
    }

    uint16_t payload_len = 0;
    esp_err_t build_err = build_tags_payload(cfg, &payload_len);
    if (build_err != ESP_OK) {
        return build_err;
    }

    if (prev_count == 0) {
        esp_err_t clear_err = clear_all_tags_locked();
        if (clear_err != ESP_OK) {
            ESP_LOGW(TAG, "clear before full sync failed, continue anyway");
        }
    } else if (prev_count > cfg->tag_count) {
        clear_orphan_tags(cfg->tag_count, prev_count);
    }

    esp_err_t range_err = sync_detection_range(cfg);
    if (range_err != ESP_OK) {
        ESP_LOGW(TAG, "detection range sync failed, continue");
    }

    if (prev_count == 0) {
        ESP_LOGI(TAG, "full sync %u tags to C4004", cfg->tag_count);
    } else if (prev_count < cfg->tag_count) {
        ESP_LOGI(TAG, "add/update sync %u tags (was %u, no clear-all)", cfg->tag_count, prev_count);
    } else if (prev_count > cfg->tag_count) {
        ESP_LOGI(TAG, "delete sync %u tags (was %u)", cfg->tag_count, prev_count);
    } else {
        ESP_LOGI(TAG, "update sync %u tags", cfg->tag_count);
    }

    esp_err_t sync_err = send_tags_from_config(payload_len, cfg->tag_count);
    if (sync_err != ESP_OK) {
        ESP_LOGW(TAG, "sync tags timeout (%u tags, %u bytes)", cfg->tag_count, payload_len);
    }

    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    memset(s_tag_states, 0, sizeof(s_tag_states));
    memset(s_approach_hold_until, 0, sizeof(s_approach_hold_until));
    memset(s_leave_hold_until, 0, sizeof(s_leave_hold_until));
    memset(s_enter_hold_until, 0, sizeof(s_enter_hold_until));
    memset(s_exit_hold_until, 0, sizeof(s_exit_hold_until));
    xSemaphoreGive(s_state_lock);

    ESP_LOGI(TAG, "synced %u tags to C4004 OK", cfg->tag_count);
    return sync_err;
}

bool c4004_sensor_is_ready(void)
{
    return s_ready;
}

esp_err_t c4004_sensor_get_tag_status(uint8_t index, area_status_t *out)
{
    if (!s_ready || out == NULL || index >= AREA_TAG_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    const c4004_tag_state_t *st = &s_tag_states[index];
    out->active = st->active;
    out->static_count = st->static_count;
    out->enter = st->enter || signal_active(s_enter_hold_until[index]);
    out->exit = st->exit || signal_active(s_exit_hold_until[index]);
    out->approaching = st->approaching || signal_active(s_approach_hold_until[index]);
    out->leaving = st->leaving || signal_active(s_leave_hold_until[index]);
    xSemaphoreGive(s_state_lock);
    return ESP_OK;
}

esp_err_t c4004_sensor_query_target(float *x, float *y, uint8_t *target_count, int8_t *picked_index)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t query = C4004_QUERY_DATA;
    if (!request_frame(C4004_CTRL_TRAJECTORY, C4004_CMD_TRAJECTORY_QUERY_TARGET,
                       &query, 1, &s_work_packet, C4004_DEFAULT_TIMEOUT_MS)) {
        ESP_LOGW(TAG, "query target timeout");
        return ESP_ERR_TIMEOUT;
    }

    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    parse_targets(s_work_packet.data, s_work_packet.len);
    uint8_t count = s_target_count;
    if (count == 0) {
        xSemaphoreGive(s_state_lock);
        ESP_LOGD(TAG, "query target: no target detected");
        return ESP_ERR_NOT_FOUND;
    }

    int idx = pick_coord_target_index();
    if (idx < 0 || (uint8_t)idx >= count) {
        idx = 0;
    }

    if (count > 1) {
        for (uint8_t i = 0; i < count; i++) {
            ESP_LOGI(TAG, "  target[%u] idx=%u kin=0x%02X pos=(%d,%d) spd=%d cm",
                     i, s_targets[i].index, s_targets[i].kinesia,
                     (int)s_targets[i].x, (int)s_targets[i].y, (int)s_targets[i].speed);
        }
    }

    if (x != NULL) {
        *x = (float)s_targets[idx].x;
    }
    if (y != NULL) {
        *y = (float)s_targets[idx].y;
    }
    if (target_count != NULL) {
        *target_count = count;
    }
    if (picked_index != NULL) {
        *picked_index = (int8_t)idx;
    }
    xSemaphoreGive(s_state_lock);

    ESP_LOGI(TAG, "query target: count=%u picked[%d]=(%.0f, %.0f) cm",
             count, idx, (double)s_targets[idx].x, (double)s_targets[idx].y);
    return ESP_OK;
}
