/**
 * @file mcp_server.c
 * @brief 传感器 MCP 服务（基于 espressif/mcp-c-sdk）
 *
 * 模块职责：
 * 1. 注册 MCP 工具（tools/call）—— 快照、配置、雷达坐标
 * 2. 注册 MCP 资源（resources/read + subscribe）—— STCC4 温湿度、C4004 雷达事件
 * 3. 通过 SDK 内置 HTTP 传输在 8080 端口暴露 /mcp_server 端点
 *
 * 启动流程（与官方 demo 一致）：
 *   esp_mcp_create → register tools/resources → esp_mcp_mgr_init
 *   → esp_mcp_mgr_start → esp_mcp_mgr_register_endpoint
 */

#include "mcp_server.h"
#include "mcp_radar_events.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mcp_engine.h"
#include "esp_mcp_mgr.h"
#include "esp_mcp_resource.h"
#include "esp_mcp_tool.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "sensor_data.h"
#include "stcc4_sensor.h"

static const char *TAG = "mcp_server";

/* MCP 引擎与 mgr 生命周期对象 */
static esp_mcp_t *s_mcp = NULL;
static esp_mcp_mgr_handle_t s_mgr = 0;

/* 温度资源句柄：notify 时通过 esp_mcp_mgr_emit_message 推送 resources/updated */
static esp_mcp_resource_t *s_temp_resource = NULL;
static esp_mcp_resource_t *s_events_resource = NULL;

/** 随 area_config 动态注册的单区域事件资源 */
typedef struct {
    esp_mcp_resource_t *resource;
    char uri[48];
} mcp_dynamic_area_resource_t;

static mcp_dynamic_area_resource_t s_area_resources[AREA_TAG_MAX];
static uint8_t s_area_resource_count;

static QueueHandle_t s_area_sync_queue;
static TaskHandle_t s_area_sync_task;

#define MCP_AREA_SYNC_STACK 10240

/** 仅 mcp_area_sync 任务使用，避免 area_config_t 占用任务栈 */
static area_config_t s_sync_cfg;
static char s_sync_title[AREA_TAG_NAME_LEN + 16];

static bool s_running = false;
static TaskHandle_t s_temp_watch_task = NULL;

/** 温度变化超过此阈值（°C）时向订阅客户端推送 resources/updated */
#define MCP_TEMP_CHANGE_THRESHOLD_C 0.1f

/* --------------------------------------------------------------------------
 * HTTP 传输补丁（仅覆盖 register_endpoint，其余仍走 SDK）
 *
 * 背景：esp_mcp_transport_http_server 的 HEAD handler 未设置 Content-Type，
 * httpd 默认为 text/html。Hermes 连接探测要求 application/json 或
 * text/event-stream，否则会报 "not an MCP response"。
 *
 * 做法：SDK 注册完路由后，替换 /mcp_server 与 /mcp_server_sse 的 HEAD handler。
 * mcp_sdk_http_view_t 与 SDK 内部 esp_mcp_http_item_s 前两字段布局一致，
 * 用于取得 httpd_handle_t（勿修改 managed_components 内 SDK 源码）。
 * -------------------------------------------------------------------------- */
typedef struct {
    esp_mcp_mgr_handle_t mgr;
    httpd_handle_t httpd;
} mcp_sdk_http_view_t;

/** HEAD 探测响应：204 + application/json（满足 Hermes / Streamable HTTP 客户端） */
static esp_err_t mcp_head_handler_fix(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "MCP-Protocol-Version", CONFIG_MCP_HTTP_DEFAULT_PROTOCOL_VERSION);
#if CONFIG_MCP_HTTP_CORS_ENABLE
    httpd_resp_set_hdr(req, "Allow", "POST, GET, DELETE, HEAD, OPTIONS");
#else
    httpd_resp_set_hdr(req, "Allow", "POST, GET, DELETE, HEAD");
#endif
    return httpd_resp_send(req, NULL, 0);
}

/** 注销 SDK 注册的 HEAD handler，改挂 mcp_head_handler_fix */
static esp_err_t mcp_reregister_head(httpd_handle_t httpd, const char *uri)
{
    esp_err_t err = httpd_unregister_uri_handler(httpd, uri, HTTP_HEAD);
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        return err;
    }

    httpd_uri_t head_uri = {
        .uri = uri,
        .method = HTTP_HEAD,
        .handler = mcp_head_handler_fix,
        .user_ctx = NULL,
    };
    return httpd_register_uri_handler(httpd, &head_uri);
}

/**
 * 包装 SDK register_endpoint：先走标准注册，再修补 HEAD Content-Type。
 * 作为 esp_mcp_transport_t.register_endpoint 在运行时替换（见 mcp_server_start）。
 */
static esp_err_t mcp_transport_register_endpoint(esp_mcp_transport_handle_t handle,
                                                 const char *ep_name,
                                                 void *priv_data)
{
    esp_err_t err = esp_mcp_transport_http_server.register_endpoint(handle, ep_name, priv_data);
    if (err != ESP_OK) {
        return err;
    }

    mcp_sdk_http_view_t *view = (mcp_sdk_http_view_t *)handle;
    if (view == NULL || view->httpd == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    char uri[40];
    snprintf(uri, sizeof(uri), "/%s", ep_name);
    err = mcp_reregister_head(view->httpd, uri);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HEAD fix failed for %s: %s", uri, esp_err_to_name(err));
        return err;
    }

#if CONFIG_MCP_HTTP_SSE_ENABLE
    snprintf(uri, sizeof(uri), "/%s_sse", ep_name);
    err = mcp_reregister_head(view->httpd, uri);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HEAD fix failed for %s: %s", uri, esp_err_to_name(err));
    }
#endif

    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * 工具回调：JSON 构建与 sensors.* 工具实现
 * 字段与 Web API（GET /api/data、/api/config）保持一致，便于客户端复用。
 * -------------------------------------------------------------------------- */

/** 将 sensor_snapshot_t 转为 JSON（areas 内 strategy 1/2/3 字段含义见 server instructions） */
static cJSON *snapshot_to_json(const sensor_snapshot_t *snapshot)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }

    cJSON_AddBoolToObject(root, "env_ready", snapshot->env_ready);
    if (snapshot->env_ready) {
        cJSON_AddNumberToObject(root, "temperature_c", snapshot->temperature);
        cJSON_AddNumberToObject(root, "humidity_pct", snapshot->humidity);
        cJSON_AddNumberToObject(root, "co2_ppm", snapshot->co2);
    } else {
        cJSON_AddNullToObject(root, "temperature_c");
        cJSON_AddNullToObject(root, "humidity_pct");
        cJSON_AddNullToObject(root, "co2_ppm");
    }

    cJSON_AddBoolToObject(root, "lux_ready", snapshot->lux_ready);
    if (snapshot->lux_ready) {
        cJSON_AddNumberToObject(root, "lux", snapshot->lux);
    } else {
        cJSON_AddNullToObject(root, "lux");
    }

    cJSON_AddBoolToObject(root, "accel_ready", snapshot->accel_ready);
    if (snapshot->accel_ready) {
        cJSON_AddNumberToObject(root, "ax_g", snapshot->ax);
        cJSON_AddNumberToObject(root, "ay_g", snapshot->ay);
        cJSON_AddNumberToObject(root, "az_g", snapshot->az);
    } else {
        cJSON_AddNullToObject(root, "ax_g");
        cJSON_AddNullToObject(root, "ay_g");
        cJSON_AddNullToObject(root, "az_g");
    }

    cJSON *areas = cJSON_AddArrayToObject(root, "areas");
    for (uint8_t i = 0; i < snapshot->area_count; i++) {
        const area_status_t *st = &snapshot->areas[i];
        cJSON *area = cJSON_CreateObject();
        if (area == NULL) {
            continue;
        }

        cJSON_AddStringToObject(area, "name", st->name);
        cJSON_AddNumberToObject(area, "strategy", st->strategy);

        switch (st->strategy) {
        case 2: /* 边界：进入 / 离开 */
            cJSON_AddBoolToObject(area, "enter", st->enter);
            cJSON_AddBoolToObject(area, "exit", st->exit);
            break;
        case 3: /* 接近 / 远离 */
            cJSON_AddBoolToObject(area, "approaching", st->approaching);
            cJSON_AddBoolToObject(area, "leaving", st->leaving);
            break;
        default: /* strategy 1：人数统计 */
            cJSON_AddNumberToObject(area, "active", st->active);
            cJSON_AddNumberToObject(area, "static", st->static_count);
            break;
        }

        cJSON_AddItemToArray(areas, area);
    }

    return root;
}

/** C4004 区域配置 → JSON（tags 含四角坐标） */
static cJSON *area_config_to_json(const area_config_t *cfg)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }

    cJSON_AddBoolToObject(root, "enabled", cfg->enabled);
    cJSON *tags = cJSON_AddArrayToObject(root, "tags");
    for (uint8_t i = 0; i < cfg->tag_count; i++) {
        const area_tag_t *tag = &cfg->tags[i];
        cJSON *item = cJSON_CreateObject();
        if (item == NULL) {
            continue;
        }

        cJSON_AddNumberToObject(item, "id", i);
        cJSON_AddNumberToObject(item, "strategy", tag->strategy);
        cJSON_AddStringToObject(item, "name", tag->name);

        cJSON *coords = cJSON_AddArrayToObject(item, "coords");
        for (int p = 0; p < 4; p++) {
            cJSON *pt = cJSON_CreateArray();
            if (pt == NULL) {
                continue;
            }
            cJSON_AddItemToArray(pt, cJSON_CreateNumber(tag->coords[p][0]));
            cJSON_AddItemToArray(pt, cJSON_CreateNumber(tag->coords[p][1]));
            cJSON_AddItemToArray(coords, pt);
        }

        cJSON_AddItemToArray(tags, item);
    }

    return root;
}

/** cJSON → esp_mcp_value 字符串（工具返回值统一为 JSON 字符串） */
static esp_mcp_value_t json_to_mcp_value(cJSON *json)
{
    if (json == NULL) {
        return esp_mcp_value_create_bool(false);
    }

    char *payload = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (payload == NULL) {
        return esp_mcp_value_create_bool(false);
    }

    esp_mcp_value_t result = esp_mcp_value_create_string(payload);
    free(payload);
    if (result.type == ESP_MCP_VALUE_TYPE_INVALID) {
        return esp_mcp_value_create_bool(false);
    }
    return result;
}

static esp_mcp_value_t get_snapshot_callback(const esp_mcp_property_list_t *properties)
{
    (void)properties;

    sensor_snapshot_t snapshot = {0};
    if (sensor_data_get_snapshot(&snapshot) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read sensor snapshot");
        return esp_mcp_value_create_bool(false);
    }

    return json_to_mcp_value(snapshot_to_json(&snapshot));
}

static esp_mcp_value_t get_config_callback(const esp_mcp_property_list_t *properties)
{
    (void)properties;

    area_config_t cfg = {0};
    if (area_config_get(&cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read area configuration");
        return esp_mcp_value_create_bool(false);
    }

    return json_to_mcp_value(area_config_to_json(&cfg));
}

static esp_mcp_value_t fetch_radar_coord_callback(const esp_mcp_property_list_t *properties)
{
    (void)properties;

    float x = 0.0f;
    float y = 0.0f;
    uint8_t target_count = 0;
    int8_t picked_index = -1;

    if (sensor_data_fetch_radar_coord(&x, &y, &target_count, &picked_index) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to query radar coordinates");
        return esp_mcp_value_create_bool(false);
    }

    cJSON *json = cJSON_CreateObject();
    if (json == NULL) {
        return esp_mcp_value_create_bool(false);
    }

    cJSON_AddNumberToObject(json, "x_cm", x);
    cJSON_AddNumberToObject(json, "y_cm", y);
    cJSON_AddNumberToObject(json, "target_count", target_count);
    cJSON_AddNumberToObject(json, "picked_index", picked_index);

    return json_to_mcp_value(json);
}

/* --------------------------------------------------------------------------
 * MCP 资源：sensor://temperature
 *
 * 客户端流程：resources/subscribe → 保持 GET SSE → 收到
 * notifications/resources/updated → resources/read 取最新值。
 * SSE 推送由 SDK transport 负责；本模块仅在温度变化时触发 notify。
 * -------------------------------------------------------------------------- */

/** resources/read 回调：返回 STCC4 最近一次采样 */
static esp_err_t temperature_resource_read_cb(const char *uri,
                                              char **out_mime_type,
                                              char **out_text,
                                              char **out_blob_base64,
                                              void *user_ctx)
{
    (void)uri;
    (void)user_ctx;
    (void)out_blob_base64;

    float temperature = 0.0f;
    float humidity = 0.0f;
    int co2 = 0;

    cJSON *json = cJSON_CreateObject();
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (stcc4_sensor_get(&temperature, &humidity, &co2) == ESP_OK) {
        cJSON_AddBoolToObject(json, "ready", true);
        cJSON_AddNumberToObject(json, "temperature_c", temperature);
        cJSON_AddNumberToObject(json, "humidity_pct", humidity);
        cJSON_AddNumberToObject(json, "co2_ppm", co2);
    } else {
        cJSON_AddBoolToObject(json, "ready", false);
    }

    char *payload = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (payload == NULL) {
        return ESP_ERR_NO_MEM;
    }

    *out_mime_type = strdup("application/json");
    *out_text = payload;
    if (*out_mime_type == NULL) {
        free(payload);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/** 全局雷达事件流 read 回调 */
static esp_err_t radar_events_feed_read_cb(const char *uri,
                                           char **out_mime_type,
                                           char **out_text,
                                           char **out_blob_base64,
                                           void *user_ctx)
{
    (void)uri;
    (void)user_ctx;
    (void)out_blob_base64;

    char *payload = NULL;
    if (mcp_radar_events_read_feed(&payload) != ESP_OK || payload == NULL) {
        return ESP_ERR_NO_MEM;
    }

    *out_mime_type = strdup("application/json");
    *out_text = payload;
    if (*out_mime_type == NULL) {
        free(payload);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/** 单区域事件 read 回调（user_ctx 为区域下标） */
static esp_err_t radar_area_events_read_cb(const char *uri,
                                           char **out_mime_type,
                                           char **out_text,
                                           char **out_blob_base64,
                                           void *user_ctx)
{
    (void)uri;
    (void)out_blob_base64;

    uint8_t area_id = (uint8_t)(uintptr_t)user_ctx;
    char *payload = NULL;
    esp_err_t err = mcp_radar_events_read_area(area_id, &payload);

    cJSON *json = NULL;
    if (err == ESP_ERR_NOT_FOUND) {
        json = cJSON_CreateObject();
        if (json != NULL) {
            cJSON_AddBoolToObject(json, "ready", false);
            cJSON_AddNumberToObject(json, "area_id", area_id);
        }
    } else if (err == ESP_OK && payload != NULL) {
        *out_text = payload;
        *out_mime_type = strdup("application/json");
        if (*out_mime_type == NULL) {
            free(payload);
            return ESP_ERR_NO_MEM;
        }
        return ESP_OK;
    }

    if (json == NULL) {
        free(payload);
        return err == ESP_OK ? ESP_ERR_NO_MEM : err;
    }

    payload = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (payload == NULL) {
        return ESP_ERR_NO_MEM;
    }

    *out_mime_type = strdup("application/json");
    *out_text = payload;
    if (*out_mime_type == NULL) {
        free(payload);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void clear_area_resources(void)
{
    if (s_mcp == NULL) {
        s_area_resource_count = 0;
        return;
    }

    for (uint8_t i = 0; i < s_area_resource_count; i++) {
        if (s_area_resources[i].resource != NULL) {
            esp_mcp_remove_resource(s_mcp, s_area_resources[i].resource);
            esp_mcp_resource_destroy(s_area_resources[i].resource);
            s_area_resources[i].resource = NULL;
            s_area_resources[i].uri[0] = '\0';
        }
    }
    s_area_resource_count = 0;
    memset(s_area_resources, 0, sizeof(s_area_resources));
}

/** 按当前 area_config 重建 sensor://area/{id}/events 资源（触发 resources/list_changed） */
static void mcp_server_sync_area_resources(void)
{
    if (s_mcp == NULL) {
        return;
    }

    clear_area_resources();

    if (area_config_get(&s_sync_cfg) != ESP_OK || !s_sync_cfg.enabled || s_sync_cfg.tag_count == 0) {
        ESP_LOGI(TAG, "area event resources cleared (radar areas disabled or empty)");
        return;
    }

    for (uint8_t i = 0; i < s_sync_cfg.tag_count; i++) {
        const area_tag_t *tag = &s_sync_cfg.tags[i];
        mcp_dynamic_area_resource_t *slot = &s_area_resources[i];

        snprintf(slot->uri, sizeof(slot->uri), MCP_RESOURCE_AREA_EVENTS_URI_FMT, i);

        snprintf(s_sync_title, sizeof(s_sync_title), "Events: %s", tag->name);

        static const char description[] =
            "Radar events for this tag area. "
            "Subscribe; read after notifications/resources/updated.";

        slot->resource = esp_mcp_resource_create(
            slot->uri,
            s_sync_title,
            s_sync_title,
            description,
            "application/json",
            radar_area_events_read_cb,
            (void *)(uintptr_t)i);
        if (slot->resource == NULL) {
            ESP_LOGE(TAG, "create area resource failed for id=%u", i);
            continue;
        }

        esp_err_t err = esp_mcp_add_resource(s_mcp, slot->resource);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "add area resource failed id=%u: %s", i, esp_err_to_name(err));
            esp_mcp_resource_destroy(slot->resource);
            slot->resource = NULL;
            slot->uri[0] = '\0';
            continue;
        }
    }

    s_area_resource_count = s_sync_cfg.tag_count;
    ESP_LOGI(TAG, "synced %u dynamic area event resources", s_area_resource_count);
}

static void mcp_server_run_area_sync(void)
{
    mcp_server_sync_area_resources();
    mcp_server_notify_resource(MCP_RESOURCE_EVENTS_URI);
}

static void mcp_area_sync_task(void *arg)
{
    (void)arg;

    while (true) {
        uint8_t trigger = 0;
        if (xQueueReceive(s_area_sync_queue, &trigger, pdMS_TO_TICKS(500)) != pdTRUE) {
            if (!s_running) {
                break;
            }
            continue;
        }

        /* 合并短时间内的多次 Web 配置变更 */
        while (xQueueReceive(s_area_sync_queue, &trigger, 0) == pdTRUE) {
        }

        if (!s_running) {
            break;
        }

        mcp_server_run_area_sync();
    }

    s_area_sync_task = NULL;
    vTaskDelete(NULL);
}

static void mcp_server_request_area_sync(void)
{
    if (s_area_sync_queue == NULL) {
        return;
    }

    uint8_t trigger = 1;
    (void)xQueueSend(s_area_sync_queue, &trigger, 0);
}

static void mcp_server_on_area_config_changed(void)
{
    mcp_radar_events_reset_state();
    mcp_radar_events_clear_log();
    mcp_server_request_area_sync();
}

static void mcp_server_on_radar_event(uint8_t area_id)
{
    if (area_id == MCP_EVENT_AREA_ID_GLOBAL) {
        /* 独立 room_empty（非与 area_exit 合并时）仅通知全局流 */
        mcp_server_notify_resource(MCP_RESOURCE_EVENTS_URI);
        return;
    }
    if (area_id < s_area_resource_count && s_area_resources[area_id].resource != NULL) {
        mcp_server_notify_resource(s_area_resources[area_id].uri);
        return;
    }
    mcp_server_notify_resource(MCP_RESOURCE_EVENTS_URI);
}

/**
 * 通知订阅者资源已更新。
 * 勿用 remove+add：SDK 在 remove 与 add 时各发一次 resources/updated，
 * 客户端会对同一 URI 连续 read 两次，表现为重复上报。
 */
static esp_mcp_resource_t *resource_handle_for_uri(const char *uri)
{
    if (uri == NULL) {
        return NULL;
    }
    if (strcmp(uri, MCP_RESOURCE_TEMPERATURE_URI) == 0) {
        return s_temp_resource;
    }
    if (strcmp(uri, MCP_RESOURCE_EVENTS_URI) == 0) {
        return s_events_resource;
    }
    for (uint8_t i = 0; i < s_area_resource_count; i++) {
        if (s_area_resources[i].resource != NULL &&
            strcmp(uri, s_area_resources[i].uri) == 0) {
            return s_area_resources[i].resource;
        }
    }
    return NULL;
}

void mcp_server_notify_resource(const char *uri)
{
    if (!s_running || s_mgr == 0 || uri == NULL) {
        return;
    }

    if (resource_handle_for_uri(uri) == NULL) {
        return;
    }

    char msg[160];
    int n = snprintf(msg, sizeof(msg),
                     "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/resources/updated\","
                     "\"params\":{\"uri\":\"%s\"}}",
                     uri);
    if (n <= 0 || (size_t)n >= sizeof(msg)) {
        ESP_LOGW(TAG, "Resource notify truncated: %s", uri);
        return;
    }

    esp_err_t err = esp_mcp_mgr_emit_message(s_mgr, NULL, msg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Resource notify failed (%s): %s", uri, esp_err_to_name(err));
    }
}

static void mcp_server_notify_resource_updated(const char *uri)
{
    mcp_server_notify_resource(uri);
}

/** 后台轮询 STCC4，温度变化 ≥ MCP_TEMP_CHANGE_THRESHOLD_C 时推送订阅通知 */
static void temperature_watch_task(void *arg)
{
    (void)arg;
    float last_temperature = NAN;
    const TickType_t interval = pdMS_TO_TICKS(CONFIG_STCC4_POLL_INTERVAL_MS);

    while (s_running) {
        float temperature = 0.0f;
        float humidity = 0.0f;
        int co2 = 0;

        if (stcc4_sensor_get(&temperature, &humidity, &co2) == ESP_OK) {
            bool changed = isnan(last_temperature) ||
                           fabsf(temperature - last_temperature) >= MCP_TEMP_CHANGE_THRESHOLD_C;
            if (changed) {
                last_temperature = temperature;
                mcp_server_notify_resource_updated(MCP_RESOURCE_TEMPERATURE_URI);
                ESP_LOGI(TAG, "Temperature update pushed: %.2f C", temperature);
            }
        }

        vTaskDelay(interval);
    }

    s_temp_watch_task = NULL;
    vTaskDelete(NULL);
}

/** 注册 MCP 资源（温度 + 动态 C4004 雷达事件） */
static esp_err_t register_resources(void)
{
    esp_mcp_resource_t *resource = esp_mcp_resource_create(
        MCP_RESOURCE_TEMPERATURE_URI,
        "temperature",
        "Temperature sensor",
        "Live STCC4 temperature, humidity, and CO2 readings. "
        "Subscribe to receive notifications/resources/updated when temperature changes.",
        "application/json",
        temperature_resource_read_cb,
        NULL);
    ESP_RETURN_ON_FALSE(resource, ESP_ERR_NO_MEM, TAG, "create temperature resource failed");
    s_temp_resource = resource;
    ESP_RETURN_ON_ERROR(esp_mcp_add_resource(s_mcp, resource), TAG, "add temperature resource failed");

    resource = esp_mcp_resource_create(
        MCP_RESOURCE_EVENTS_URI,
        "radar-events",
        "Radar event stream",
        "All C4004 radar events with current area list. "
        "Areas update automatically when tags are added/removed in Web config. "
        "Also subscribe to sensor://area/{id}/events for a single area.",
        "application/json",
        radar_events_feed_read_cb,
        NULL);
    ESP_RETURN_ON_FALSE(resource, ESP_ERR_NO_MEM, TAG, "create events resource failed");
    s_events_resource = resource;
    ESP_RETURN_ON_ERROR(esp_mcp_add_resource(s_mcp, resource), TAG, "add events resource failed");

    return ESP_OK;
}

/** 注册三个 MCP 工具（Hermes 中可见为 sensors.get_snapshot 等） */
static esp_err_t register_tools(void)
{
    
    esp_mcp_tool_t *tool = esp_mcp_tool_create(
        "sensors.get_snapshot",
        "Read STCC4 environment data and C4004 area status (same fields as GET /api/data).",
        get_snapshot_callback);
    ESP_RETURN_ON_FALSE(tool, ESP_ERR_NO_MEM, TAG, "create snapshot tool failed");
    ESP_RETURN_ON_ERROR(esp_mcp_add_tool(s_mcp, tool), TAG, "add snapshot tool failed");

    tool = esp_mcp_tool_create(
        "sensors.get_config",
        "Read persisted C4004 area tags, strategies, and corner coordinates.",
        get_config_callback);
    ESP_RETURN_ON_FALSE(tool, ESP_ERR_NO_MEM, TAG, "create config tool failed");
    ESP_RETURN_ON_ERROR(esp_mcp_add_tool(s_mcp, tool), TAG, "add config tool failed");

    tool = esp_mcp_tool_create(
        "sensors.fetch_radar_coord",
        "Query current C4004 target position; when multiple targets exist, prefer motion.",
        fetch_radar_coord_callback);
    ESP_RETURN_ON_FALSE(tool, ESP_ERR_NO_MEM, TAG, "create coord tool failed");
    ESP_RETURN_ON_ERROR(esp_mcp_add_tool(s_mcp, tool), TAG, "add coord tool failed");

    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * 生命周期：init / start / stop
 * -------------------------------------------------------------------------- */

esp_err_t mcp_server_start(void)
{
    if (s_running) {
        return ESP_OK;
    }

    esp_err_t err = esp_mcp_create(&s_mcp);
    ESP_RETURN_ON_ERROR(err, TAG, "esp_mcp_create failed");

    err = register_tools();
    if (err != ESP_OK) {
        esp_mcp_destroy(s_mcp);
        s_mcp = NULL;
        return err;
    }

    err = register_resources();
    if (err != ESP_OK) {
        esp_mcp_destroy(s_mcp);
        s_mcp = NULL;
        return err;
    }

    ESP_ERROR_CHECK(esp_mcp_set_server_info(
        s_mcp,
        "MCP Sensors",
        "ESP32-P4 sensor hub with STCC4 environment sensing and C4004 mmWave radar.",
        NULL,
        NULL));
    ESP_ERROR_CHECK(esp_mcp_set_instructions(
        s_mcp,
        "Use sensors.get_snapshot for live readings. "
        "Subscribe to sensor://temperature for push updates when temperature changes. "
        "Subscribe to sensor://events for all radar events; areas[] reflects current Web config. "
        "Per-area: sensor://area/{id}/events (id matches sensors.get_config tags). "
        "Event types: room_empty, area_enter, area_exit, area_dwell (dwell after continuous occupancy). "
        "Read after notifications/resources/updated. "
        "Strategy 1=people count (+1 enter/-1 exit per person), 2=boundary enter/exit, 3=approach/leave."));

    /*
     * 独立 httpd 监听 8080。ctrl_port 必须不同于 Web httpd（默认 32768），
     * 否则 esp_mcp_mgr_start → httpd_start 会失败。
     * max_uri_handlers 需容纳 /mcp_server + /mcp_server_sse 及 CORS/SSE 路由。
     */
    httpd_config_t http_config = HTTPD_DEFAULT_CONFIG();
    http_config.server_port = MCP_HTTP_PORT;
    http_config.ctrl_port += 1;
    http_config.stack_size = 8192;
    http_config.max_uri_handlers = 48;
    http_config.max_open_sockets = 7;
    http_config.lru_purge_enable = true;

    ESP_LOGI(TAG, "Starting MCP HTTP server on port %d (ctrl_port %d)",
             http_config.server_port, http_config.ctrl_port);

    /* 复制 SDK transport 表，仅替换 register_endpoint（见文件顶部说明） */
    esp_mcp_transport_t transport = esp_mcp_transport_http_server;
    transport.register_endpoint = mcp_transport_register_endpoint;

    esp_mcp_mgr_config_t mgr_config = {
        .transport = transport,
        .config = &http_config,
        .instance = s_mcp,
    };

    err = esp_mcp_mgr_init(mgr_config, &s_mgr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_mcp_mgr_init failed: %s", esp_err_to_name(err));
        esp_mcp_destroy(s_mcp);
        s_mcp = NULL;
        return err;
    }

    err = esp_mcp_mgr_start(s_mgr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_mcp_mgr_start failed: %s (check port %d / ctrl_port conflict)",
                 esp_err_to_name(err), MCP_HTTP_PORT);
        esp_mcp_mgr_deinit(s_mgr);
        s_mgr = 0;
        esp_mcp_destroy(s_mcp);
        s_mcp = NULL;
        return err;
    }

    err = esp_mcp_mgr_register_endpoint(s_mgr, MCP_ENDPOINT_NAME, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register endpoint failed: %s (max_uri_handlers=%d?)",
                 esp_err_to_name(err), http_config.max_uri_handlers);
        esp_mcp_mgr_stop(s_mgr);
        esp_mcp_mgr_deinit(s_mgr);
        s_mgr = 0;
        esp_mcp_destroy(s_mcp);
        s_mcp = NULL;
        return err;
    }

    ESP_LOGI(TAG, "MCP endpoint registered: http://<device-ip>:%d/%s",
             MCP_HTTP_PORT, MCP_ENDPOINT_NAME);

    s_running = true;

    s_area_sync_queue = xQueueCreate(4, sizeof(uint8_t));
    if (s_area_sync_queue == NULL) {
        ESP_LOGE(TAG, "area sync queue create failed");
        s_running = false;
        esp_mcp_mgr_unregister_endpoint(s_mgr, MCP_ENDPOINT_NAME);
        esp_mcp_mgr_stop(s_mgr);
        esp_mcp_mgr_deinit(s_mgr);
        s_mgr = 0;
        esp_mcp_destroy(s_mcp);
        s_mcp = NULL;
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(mcp_area_sync_task, "mcp_area_sync", MCP_AREA_SYNC_STACK,
                    NULL, 3, &s_area_sync_task) != pdPASS) {
        ESP_LOGE(TAG, "area sync task create failed");
        vQueueDelete(s_area_sync_queue);
        s_area_sync_queue = NULL;
        s_running = false;
        esp_mcp_mgr_unregister_endpoint(s_mgr, MCP_ENDPOINT_NAME);
        esp_mcp_mgr_stop(s_mgr);
        esp_mcp_mgr_deinit(s_mgr);
        s_mgr = 0;
        esp_mcp_destroy(s_mcp);
        s_mcp = NULL;
        return ESP_ERR_NO_MEM;
    }

    area_config_set_change_callback(mcp_server_on_area_config_changed);
    mcp_server_request_area_sync();

    if (xTaskCreate(temperature_watch_task, "mcp_temp_watch", 3072, NULL, 4, &s_temp_watch_task) != pdPASS) {
        ESP_LOGW(TAG, "Failed to start temperature watch task");
        s_temp_watch_task = NULL;
    }

    err = mcp_radar_events_start(mcp_server_on_radar_event);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to start radar event watch: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "MCP ready at http://<device-ip>:%d%s", MCP_HTTP_PORT, MCP_ENDPOINT_URI);
    return ESP_OK;
}

void mcp_server_stop(void)
{
    if (!s_running) {
        return;
    }

    s_running = false;
    area_config_set_change_callback(NULL);

    if (s_area_sync_queue != NULL) {
        uint8_t trigger = 1;
        (void)xQueueSend(s_area_sync_queue, &trigger, 0);
    }
    while (s_area_sync_task != NULL) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (s_area_sync_queue != NULL) {
        vQueueDelete(s_area_sync_queue);
        s_area_sync_queue = NULL;
    }

    mcp_radar_events_stop();
    clear_area_resources();
    /* 等待温度监测任务退出（s_running 置 false 后任务会自行结束） */
    while (s_temp_watch_task != NULL) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    esp_mcp_mgr_unregister_endpoint(s_mgr, MCP_ENDPOINT_NAME);
    esp_mcp_mgr_stop(s_mgr);
    esp_mcp_mgr_deinit(s_mgr);
    s_mgr = 0;

    esp_mcp_destroy(s_mcp);
    s_mcp = NULL;

    ESP_LOGI(TAG, "MCP server stopped");
}

bool mcp_server_is_running(void)
{
    return s_running;
}
