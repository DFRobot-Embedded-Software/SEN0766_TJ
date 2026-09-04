/**
 * @file coap_hub.c
 * @brief 最小 UDP CoAP（非 libcoap）。资源表与字段见 coap_hub.h。
 *        改 path 必须同步 mcp_gw/coap_client.py、HA coap.py/coordinator.py。
 */
#include "coap_hub.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "cJSON.h"
#include "device_id.h"
#include "device_event_bus.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "radar_area.h"
#include "sensor_settings.h"
#include "snapshot_json.h"

static const char *TAG = "coap_hub";

#define COAP_PORT 5683
#define COAP_GET 1
#define COAP_PUT 3
#define COAP_CONTENT 69
#define COAP_CHANGED 68
#define COAP_BAD_REQ 128
#define COAP_NOT_FOUND 132
#define COAP_METHOD_NS 133
#define OPT_URI_PATH 11
#define OPT_CONTENT_FORMAT 12
#define CF_JSON 50

static int s_sock = -1;
static TaskHandle_t s_task;
static volatile bool s_stop;

#define WEB_CM_PER_M 100.0f

static cJSON *coords_cm_to_m_json(const float coords_cm[4][2])
{
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < 4; i++) {
        cJSON *pt = cJSON_CreateArray();
        cJSON_AddItemToArray(pt, cJSON_CreateNumber(coords_cm[i][0] / WEB_CM_PER_M));
        cJSON_AddItemToArray(pt, cJSON_CreateNumber(coords_cm[i][1] / WEB_CM_PER_M));
        cJSON_AddItemToArray(arr, pt);
    }
    return arr;
}

static bool parse_quad_m(const cJSON *coords, float out_cm[4][2])
{
    if (!cJSON_IsArray(coords)) {
        return false;
    }
    for (int p = 0; p < 4; p++) {
        const cJSON *pt = cJSON_GetArrayItem(coords, p);
        if (!cJSON_IsArray(pt) || cJSON_GetArraySize(pt) < 2) {
            return false;
        }
        out_cm[p][0] = (float)cJSON_GetArrayItem(pt, 0)->valuedouble * WEB_CM_PER_M;
        out_cm[p][1] = (float)cJSON_GetArrayItem(pt, 1)->valuedouble * WEB_CM_PER_M;
    }
    return true;
}

static char *config_json_build(void)
{
    area_config_t cfg = {0};
    area_config_get(&cfg);
    cJSON *root = cJSON_CreateObject();
    cJSON *tags = cJSON_AddArrayToObject(root, "tags");
    for (uint8_t i = 0; i < cfg.tag_count; i++) {
        const area_tag_t *tag = &cfg.tags[i];
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "id", i);
        cJSON_AddNumberToObject(item, "strategy", tag->strategy);
        cJSON_AddStringToObject(item, "name", tag->name);
        cJSON_AddItemToObject(item, "coords", coords_cm_to_m_json(tag->coords));
        cJSON_AddItemToArray(tags, item);
    }
    if (cfg.has_detection_range) {
        cJSON *range = cJSON_AddObjectToObject(root, "detection_range");
        cJSON_AddItemToObject(range, "coords", coords_cm_to_m_json(cfg.detection_range));
    } else {
        cJSON_AddNullToObject(root, "detection_range");
    }
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return s;
}

static void fill_tag(const cJSON *root, area_tag_t *tag)
{
    memset(tag, 0, sizeof(*tag));
    const cJSON *strategy = cJSON_GetObjectItem(root, "strategy");
    const cJSON *name = cJSON_GetObjectItem(root, "name");
    const cJSON *coords = cJSON_GetObjectItem(root, "coords");
    if (cJSON_IsNumber(strategy)) {
        tag->strategy = (uint8_t)strategy->valueint;
    }
    if (cJSON_IsString(name) && name->valuestring) {
        strncpy(tag->name, name->valuestring, sizeof(tag->name) - 1);
    }
    (void)parse_quad_m(coords, tag->coords);
}

static esp_err_t apply_config_json(const char *body)
{
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        return ESP_ERR_INVALID_ARG;
    }
    const cJSON *action = cJSON_GetObjectItem(root, "action");
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (cJSON_IsString(action) && strcmp(action->valuestring, "set_range") == 0) {
        float range_cm[4][2] = {0};
        if (parse_quad_m(cJSON_GetObjectItem(root, "coords"), range_cm)) {
            err = area_config_set_detection_range(range_cm);
        }
    } else if (cJSON_IsString(action) && strcmp(action->valuestring, "add_tag") == 0) {
        area_tag_t tag = {0};
        fill_tag(root, &tag);
        err = area_config_add_tag(&tag, NULL);
    } else if (cJSON_IsString(action) && strcmp(action->valuestring, "update_tag") == 0) {
        const cJSON *id = cJSON_GetObjectItem(root, "id");
        area_tag_t tag = {0};
        fill_tag(root, &tag);
        if (cJSON_IsNumber(id)) {
            err = area_config_update_tag((uint8_t)id->valueint, &tag);
        }
    } else if (cJSON_IsString(action) && strcmp(action->valuestring, "remove_tag") == 0) {
        const cJSON *id = cJSON_GetObjectItem(root, "id");
        if (cJSON_IsNumber(id)) {
            err = area_config_remove_tag((uint8_t)id->valueint);
        }
    }
    cJSON_Delete(root);
    return err;
}

static int parse_path(const uint8_t *pkt, int len, char *path, size_t path_sz,
                      const uint8_t **payload, int *payload_len)
{
    if (len < 4) {
        return -1;
    }
    uint8_t tkl = pkt[0] & 0x0f;
    int pos = 4 + tkl;
    path[0] = '\0';
    uint16_t last_delta = 0;
    *payload = NULL;
    *payload_len = 0;
    while (pos < len) {
        if (pkt[pos] == 0xff) {
            pos++;
            *payload = pkt + pos;
            *payload_len = len - pos;
            break;
        }
        uint8_t nibble = pkt[pos++];
        uint16_t delta = (nibble >> 4) & 0x0f;
        uint16_t olen = nibble & 0x0f;
        if (delta == 13) {
            if (pos >= len) {
                return -1;
            }
            delta = 13 + pkt[pos++];
        } else if (delta == 14) {
            if (pos + 1 >= len) {
                return -1;
            }
            delta = 269 + ((pkt[pos] << 8) | pkt[pos + 1]);
            pos += 2;
        } else if (delta == 15) {
            return -1;
        }
        if (olen == 13) {
            if (pos >= len) {
                return -1;
            }
            olen = 13 + pkt[pos++];
        } else if (olen == 14) {
            if (pos + 1 >= len) {
                return -1;
            }
            olen = 269 + ((pkt[pos] << 8) | pkt[pos + 1]);
            pos += 2;
        } else if (olen == 15) {
            return -1;
        }
        last_delta = (uint16_t)(last_delta + delta);
        if (pos + olen > len) {
            return -1;
        }
        if (last_delta == OPT_URI_PATH) {
            size_t used = strlen(path);
            if (used + 1 + olen >= path_sz) {
                return -1;
            }
            path[used++] = '/';
            memcpy(path + used, pkt + pos, olen);
            path[used + olen] = '\0';
        }
        pos += olen;
    }
    if (path[0] == '\0') {
        strncpy(path, "/", path_sz - 1);
    }
    return 0;
}

static int build_reply(uint8_t *out, int out_sz, const uint8_t *req, int req_len,
                       uint8_t code, const char *body)
{
    uint8_t tkl = req[0] & 0x0f;
    uint8_t type = (req[0] >> 4) & 0x03;
    uint8_t rtype = (type == 0) ? 2 : 1; /* CON->ACK, else NON */
    if (4 + tkl >= out_sz) {
        return 0;
    }
    out[0] = (uint8_t)((1 << 6) | (rtype << 4) | tkl);
    out[1] = code;
    out[2] = req[2];
    out[3] = req[3];
    memcpy(out + 4, req + 4, tkl);
    int pos = 4 + tkl;
    if (body && body[0]) {
        /* Content-Format json = option 12, value 1 byte 50 */
        if (pos + 3 >= out_sz) {
            return 0;
        }
        out[pos++] = (uint8_t)((OPT_CONTENT_FORMAT << 4) | 1);
        out[pos++] = CF_JSON;
        size_t n = strlen(body);
        if (pos + 1 + (int)n > out_sz) {
            n = (size_t)(out_sz - pos - 1);
        }
        out[pos++] = 0xff;
        memcpy(out + pos, body, n);
        pos += (int)n;
    }
    return pos;
}

static void handle(const uint8_t *req, int req_len, uint8_t *tx, int *tx_len)
{
    uint8_t code = req[1];
    char path[128];
    const uint8_t *payload = NULL;
    int plen = 0;
    if (parse_path(req, req_len, path, sizeof(path), &payload, &plen) != 0) {
        *tx_len = build_reply(tx, 1024, req, req_len, COAP_BAD_REQ, NULL);
        return;
    }

    char *body = NULL;
    uint8_t rc = COAP_CONTENT;

    if (code == COAP_GET && strcmp(path, "/.well-known/core") == 0) {
        /* 发现用，不是业务数据 */
        body = strdup("</sensors/snapshot>,</sensors/config>,</sensors/thresholds>,</sensors/events>,</id>");
    } else if (code == COAP_GET && strcmp(path, "/id") == 0) {
        char id[8] = {0};
        device_id_get(id, sizeof(id));
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", id);
        body = cJSON_PrintUnformatted(o);
        cJSON_Delete(o);
    } else if (code == COAP_GET && strcmp(path, "/sensors/snapshot") == 0) {
        /* 现状问答；哭声/进门不要从这里推 */
        cJSON *o = snapshot_json_build();
        body = o ? cJSON_PrintUnformatted(o) : NULL;
        cJSON_Delete(o);
        if (!body) {
            rc = COAP_BAD_REQ;
        }
    } else if (code == COAP_GET && strcmp(path, "/sensors/thresholds") == 0) {
        cJSON *o = sensor_settings_to_json();
        body = o ? cJSON_PrintUnformatted(o) : NULL;
        cJSON_Delete(o);
    } else if (code == COAP_PUT && strcmp(path, "/sensors/thresholds") == 0) {
        /* 控制面：只允许改阈值，不要在 MQTT 做对等 PUT */
        char tmp[3072];
        int n = plen < (int)sizeof(tmp) - 1 ? plen : (int)sizeof(tmp) - 1;
        memcpy(tmp, payload ? (const char *)payload : "", n);
        tmp[n] = '\0';
        cJSON *root = cJSON_Parse(tmp);
        const cJSON *sensors = root ? cJSON_GetObjectItem(root, "sensors") : NULL;
        if (sensors == NULL) {
            sensors = root;
        }
        if (sensor_settings_from_json(sensors) == ESP_OK) {
            rc = COAP_CHANGED;
            body = strdup("{\"ok\":true}");
        } else {
            rc = COAP_BAD_REQ;
        }
        cJSON_Delete(root);
    } else if (code == COAP_GET && strcmp(path, "/sensors/config") == 0) {
        body = config_json_build();
    } else if (code == COAP_PUT && strcmp(path, "/sensors/config") == 0) {
        /* 雷达区域；coords 单位米，固件内部再换成 cm */
        char tmp[1536];
        int n = plen < (int)sizeof(tmp) - 1 ? plen : (int)sizeof(tmp) - 1;
        memcpy(tmp, payload ? (const char *)payload : "", n);
        tmp[n] = '\0';
        if (apply_config_json(tmp) == ESP_OK) {
            rc = COAP_CHANGED;
            body = strdup("{\"ok\":true}");
        } else {
            rc = COAP_BAD_REQ;
        }
    } else if (code == COAP_GET && strcmp(path, "/sensors/events") == 0) {
        /* 补拉历史；实时仍走 mcp_sensors/<id>/event */
        (void)device_event_read_feed(&body);
        if (!body) {
            rc = COAP_BAD_REQ;
        }
    } else if (code != COAP_GET && code != COAP_PUT) {
        rc = COAP_METHOD_NS;
    } else {
        rc = COAP_NOT_FOUND;
    }

    *tx_len = build_reply(tx, 2048, req, req_len, rc, body);
    free(body);
}

static uint8_t s_rx[1536];
static uint8_t s_tx[2048];

static void coap_task(void *arg)
{
    (void)arg;
    struct sockaddr_in src;
    socklen_t slen = sizeof(src);
    while (!s_stop && s_sock >= 0) {
        int n = recvfrom(s_sock, s_rx, sizeof(s_rx), 0, (struct sockaddr *)&src, &slen);
        if (n < 4) {
            continue;
        }
        if (((s_rx[0] >> 6) & 0x03) != 1) {
            continue;
        }
        int tx_len = 0;
        handle(s_rx, n, s_tx, &tx_len);
        if (tx_len > 0) {
            sendto(s_sock, s_tx, tx_len, 0, (struct sockaddr *)&src, slen);
        }
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t coap_hub_start(void)
{
    if (s_task) {
        return ESP_OK;
    }
    s_stop = false;
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        return ESP_FAIL;
    }
    int opt = 1;
    setsockopt(s_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(COAP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(s_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(s_sock);
        s_sock = -1;
        return ESP_FAIL;
    }
    /* snapshot JSON + cJSON 仍吃栈；rx/tx 已改静态。勿在此路径做雷达 UART。 */
    if (xTaskCreate(coap_task, "coap_hub", 12288, NULL, 5, &s_task) != pdPASS) {
        close(s_sock);
        s_sock = -1;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "CoAP UDP :%d GET/PUT snapshot/config/thresholds", COAP_PORT);
    return ESP_OK;
}

void coap_hub_stop(void)
{
    s_stop = true;
    if (s_sock >= 0) {
        close(s_sock);
        s_sock = -1;
    }
}
