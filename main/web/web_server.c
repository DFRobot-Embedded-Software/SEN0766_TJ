#include "web_server.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sensor_data.h"
#include "web_fs.h"
#include "captive_portal.h"
#include "wifi_manager.h"

static const char *TAG = "web_server";
static httpd_handle_t s_server = NULL;
static const char *s_index_page = "index.html";
static bool s_provision_mode = false;

typedef struct {
    char ssid[33];
    char pass[65];
} wifi_connect_params_t;

static void wifi_connect_task(void *arg)
{
    wifi_connect_params_t *params = (wifi_connect_params_t *)arg;
    vTaskDelay(pdMS_TO_TICKS(300));
    web_server_stop();
    wifi_manager_connect_sta(params->ssid, params->pass);
    free(params);
    vTaskDelete(NULL);
}

static void wifi_reprovision_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(300));
    wifi_manager_enter_reprovision();
    vTaskDelete(NULL);
}

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str == NULL) {
        return ESP_ERR_NO_MEM;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
    free(json_str);
    return err;
}

static esp_err_t send_json_error(httpd_req_t *req, const char *http_status, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "message", message);

    httpd_resp_set_status(req, http_status);
    esp_err_t err = send_json(req, root);
    cJSON_Delete(root);
    return err;
}

static int read_request_body(httpd_req_t *req, char *buf, size_t buf_len)
{
    size_t offset = 0;
    while (offset < buf_len - 1) {
        int received = httpd_req_recv(req, buf + offset, buf_len - 1 - offset);
        if (received <= 0) {
            return received;
        }
        offset += (size_t)received;
        if (offset >= (size_t)req->content_len) {
            break;
        }
    }
    buf[offset] = '\0';
    return (int)offset;
}

static esp_err_t serve_spiffs_path(httpd_req_t *req, const char *relative_path)
{
    char filepath[128];
    snprintf(filepath, sizeof(filepath), WEB_FS_MOUNT_POINT "/%s", relative_path);
    return web_fs_send_file(req, filepath);
}

/** 去掉 ?query 与 #fragment，SPIFFS 路径不含这些后缀 */
static bool static_relpath_from_uri(const char *uri, char *out, size_t out_len)
{
    if (uri[0] != '/' || uri[1] == '\0' || out_len == 0) {
        return false;
    }

    const char *start = uri + 1;
    size_t i = 0;
    while (start[i] != '\0' && start[i] != '?' && start[i] != '#' && i < out_len - 1) {
        out[i] = start[i];
        i++;
    }
    out[i] = '\0';
    return i > 0;
}

static esp_err_t static_file_handler(httpd_req_t *req)
{
    const char *uri = req->uri;

    if (strncmp(uri, "/api/", 5) == 0) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    if (strncmp(uri, "/mcp", 4) == 0) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    if (s_provision_mode && captive_portal_is_probe_uri(uri)) {
        return captive_portal_redirect(req);
    }

    if (strcmp(uri, "/") == 0) {
        return serve_spiffs_path(req, s_index_page);
    }

    char rel_path[128];
    if (!static_relpath_from_uri(uri, rel_path, sizeof(rel_path))) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }
    return serve_spiffs_path(req, rel_path);
}

static esp_err_t api_data_handler(httpd_req_t *req)
{
    sensor_snapshot_t snapshot = {0};
    sensor_data_get_snapshot(&snapshot);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "env_ready", snapshot.env_ready);

    char temp_buf[16];
    char hum_buf[16];
    if (snapshot.env_ready) {
        snprintf(temp_buf, sizeof(temp_buf), "%.1f°C", snapshot.temperature);
        snprintf(hum_buf, sizeof(hum_buf), "%.1f%%", snapshot.humidity);
        cJSON_AddStringToObject(root, "temp", temp_buf);
        cJSON_AddStringToObject(root, "humidity", hum_buf);
        cJSON_AddNumberToObject(root, "co2", snapshot.co2);
    } else {
        cJSON_AddStringToObject(root, "temp", "--");
        cJSON_AddStringToObject(root, "humidity", "--");
        cJSON_AddNullToObject(root, "co2");
    }

    cJSON_AddBoolToObject(root, "lux_ready", snapshot.lux_ready);
    if (snapshot.lux_ready) {
        cJSON_AddNumberToObject(root, "lux", snapshot.lux);
    } else {
        cJSON_AddNullToObject(root, "lux");
    }

    cJSON_AddBoolToObject(root, "accel_ready", snapshot.accel_ready);
    if (snapshot.accel_ready) {
        cJSON_AddNumberToObject(root, "ax", snapshot.ax);
        cJSON_AddNumberToObject(root, "ay", snapshot.ay);
        cJSON_AddNumberToObject(root, "az", snapshot.az);
    } else {
        cJSON_AddNullToObject(root, "ax");
        cJSON_AddNullToObject(root, "ay");
        cJSON_AddNullToObject(root, "az");
    }

    cJSON *areas = cJSON_AddArrayToObject(root, "areas");
    for (uint8_t i = 0; i < snapshot.area_count; i++) {
        const area_status_t *st = &snapshot.areas[i];
        cJSON *area = cJSON_CreateObject();
        cJSON_AddStringToObject(area, "name", st->name);
        cJSON_AddNumberToObject(area, "strategy", st->strategy);

        switch (st->strategy) {
        case 2:
            cJSON_AddBoolToObject(area, "enter", st->enter);
            cJSON_AddBoolToObject(area, "exit", st->exit);
            break;
        case 3:
            cJSON_AddBoolToObject(area, "approaching", st->approaching);
            cJSON_AddBoolToObject(area, "leaving", st->leaving);
            break;
        default:
            cJSON_AddNumberToObject(area, "active", st->active);
            cJSON_AddNumberToObject(area, "static", st->static_count);
            break;
        }
        cJSON_AddItemToArray(areas, area);
    }

    esp_err_t err = send_json(req, root);
    cJSON_Delete(root);
    return err;
}

static cJSON *area_tags_to_json(const area_config_t *cfg)
{
    cJSON *tags = cJSON_CreateArray();
    for (uint8_t i = 0; i < cfg->tag_count; i++) {
        const area_tag_t *tag = &cfg->tags[i];
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "id", i);
        cJSON_AddNumberToObject(item, "strategy", tag->strategy);
        cJSON_AddStringToObject(item, "name", tag->name);
        cJSON *coords = cJSON_AddArrayToObject(item, "coords");
        for (int p = 0; p < 4; p++) {
            cJSON *pt = cJSON_CreateArray();
            cJSON_AddItemToArray(pt, cJSON_CreateNumber(tag->coords[p][0]));
            cJSON_AddItemToArray(pt, cJSON_CreateNumber(tag->coords[p][1]));
            cJSON_AddItemToArray(coords, pt);
        }
        cJSON_AddItemToArray(tags, item);
    }
    return tags;
}

static bool parse_tag_coords(const cJSON *coords, area_tag_t *tag)
{
    if (!cJSON_IsArray(coords)) {
        return false;
    }
    for (int p = 0; p < 4; p++) {
        const cJSON *pt = cJSON_GetArrayItem(coords, p);
        if (pt == NULL || !cJSON_IsArray(pt) || cJSON_GetArraySize(pt) < 2) {
            tag->coords[p][0] = 0.0f;
            tag->coords[p][1] = 0.0f;
            continue;
        }
        const cJSON *jx = cJSON_GetArrayItem(pt, 0);
        const cJSON *jy = cJSON_GetArrayItem(pt, 1);
        tag->coords[p][0] = cJSON_IsNumber(jx) ? (float)jx->valuedouble : 0.0f;
        tag->coords[p][1] = cJSON_IsNumber(jy) ? (float)jy->valuedouble : 0.0f;
    }
    return true;
}

static esp_err_t api_config_get_handler(httpd_req_t *req)
{
    area_config_t cfg = {0};
    area_config_get(&cfg);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "enabled", cfg.enabled);
    cJSON_AddItemToObject(root, "tags", area_tags_to_json(&cfg));

    esp_err_t err = send_json(req, root);
    cJSON_Delete(root);
    return err;
}

static esp_err_t api_config_post_handler(httpd_req_t *req)
{
    char body[768] = {0};
    if (read_request_body(req, body, sizeof(body)) <= 0) {
        return send_json_error(req, "400 Bad Request", "invalid body");
    }

    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        return send_json_error(req, "400 Bad Request", "invalid json");
    }

    const cJSON *action = cJSON_GetObjectItem(root, "action");
    esp_err_t save_err = ESP_OK;
    const char *message = "操作成功";

    if (cJSON_IsString(action) && strcmp(action->valuestring, "enable") == 0) {
        const cJSON *enabled = cJSON_GetObjectItem(root, "enabled");
        if (!cJSON_IsBool(enabled)) {
            cJSON_Delete(root);
            return send_json_error(req, "400 Bad Request", "missing enabled");
        }
        save_err = area_config_set_enabled(cJSON_IsTrue(enabled));
        message = cJSON_IsTrue(enabled) ? "区域配置已启用" : "区域配置已关闭";
    } else if (cJSON_IsString(action) && strcmp(action->valuestring, "add_tag") == 0) {
        const cJSON *strategy = cJSON_GetObjectItem(root, "strategy");
        const cJSON *name = cJSON_GetObjectItem(root, "name");
        const cJSON *coords = cJSON_GetObjectItem(root, "coords");
        if (!cJSON_IsNumber(strategy) || !cJSON_IsString(name) || name->valuestring[0] == '\0') {
            cJSON_Delete(root);
            return send_json_error(req, "400 Bad Request", "missing tag fields");
        }
        area_tag_t tag = {0};
        tag.strategy = (uint8_t)strategy->valueint;
        strncpy(tag.name, name->valuestring, sizeof(tag.name) - 1);
        parse_tag_coords(coords, &tag);
        if (!area_coords_valid(&tag)) {
            cJSON_Delete(root);
            return send_json_error(req, "400 Bad Request",
                                   "请先获取 4 个有效坐标点（区域宽高至少 10cm）");
        }
        save_err = area_config_add_tag(&tag);
        message = "区域标签已添加";
    } else if (cJSON_IsString(action) && strcmp(action->valuestring, "delete_tag") == 0) {
        const cJSON *id = cJSON_GetObjectItem(root, "id");
        if (!cJSON_IsNumber(id)) {
            cJSON_Delete(root);
            return send_json_error(req, "400 Bad Request", "missing tag id");
        }
        save_err = area_config_remove_tag((uint8_t)id->valueint);
        message = "区域标签已删除";
    } else {
        cJSON_Delete(root);
        return send_json_error(req, "400 Bad Request", "unknown action");
    }

    cJSON_Delete(root);

    if (save_err != ESP_OK) {
        return send_json_error(req, "500 Internal Server Error", "save failed");
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddStringToObject(resp, "message", message);
    esp_err_t err = send_json(req, resp);
    cJSON_Delete(resp);
    return err;
}

/** 从 C4004 读取当前目标坐标，供 Web 采集区域角点 */
static esp_err_t api_coord_handler(httpd_req_t *req)
{
    const char *point_str = NULL;
    if (httpd_req_get_url_query_str(req, NULL, 0) > 0) {
        char query[32];
        if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
            char value[8];
            if (httpd_query_key_value(query, "point", value, sizeof(value)) == ESP_OK) {
                point_str = value;
            }
        }
    }

    int point = point_str ? atoi(point_str) : 0;
    if (point < 1 || point > 4) {
        return send_json_error(req, "400 Bad Request", "point must be 1-4");
    }

    float x = 0.0f;
    float y = 0.0f;
    uint8_t target_count = 0;
    int8_t picked_index = -1;
    esp_err_t err = sensor_data_fetch_radar_coord(&x, &y, &target_count, &picked_index);

    cJSON *root = cJSON_CreateObject();
    if (err == ESP_OK) {
        cJSON_AddBoolToObject(root, "ok", true);
        cJSON_AddNumberToObject(root, "point", point);
        cJSON_AddNumberToObject(root, "x", x);
        cJSON_AddNumberToObject(root, "y", y);
        cJSON_AddNumberToObject(root, "targets", target_count);
        if (picked_index >= 0) {
            cJSON_AddNumberToObject(root, "picked", picked_index);
        }
        if (target_count > 1) {
            cJSON_AddStringToObject(root, "message",
                                    "检测到多个目标，已选用运动目标；标定时请仅一人在角点");
        } else {
            cJSON_AddStringToObject(root, "message", "已从 C4004 读取目标坐标");
        }
        ESP_LOGI(TAG, "coord point=%d x=%.0f y=%.0f targets=%u picked=%d",
                 point, x, y, target_count, (int)picked_index);
    } else if (err == ESP_ERR_NOT_FOUND) {
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON_AddNumberToObject(root, "point", point);
        cJSON_AddStringToObject(root, "message", "未检测到目标，请站在角点位置后重试");
        ESP_LOGW(TAG, "coord point=%d: no target", point);
    } else if (err == ESP_ERR_INVALID_STATE) {
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON_AddNumberToObject(root, "point", point);
        cJSON_AddStringToObject(root, "message", "C4004 未就绪，请检查接线与串口日志");
    } else {
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON_AddNumberToObject(root, "point", point);
        cJSON_AddStringToObject(root, "message", "读取坐标超时，请重试");
    }

    esp_err_t send_err = send_json(req, root);
    cJSON_Delete(root);
    return send_err;
}

static esp_err_t api_wifi_get_handler(httpd_req_t *req)
{
    wifi_status_t st = {0};
    wifi_manager_get_status(&st);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "configured", st.configured);
    cJSON_AddBoolToObject(root, "connected", st.connected);
    cJSON_AddBoolToObject(root, "ap_mode", st.ap_mode);
    cJSON_AddStringToObject(root, "ssid", st.ssid);
    cJSON_AddStringToObject(root, "password", st.password);
    cJSON_AddStringToObject(root, "ip", st.ip);

    esp_err_t err = send_json(req, root);
    cJSON_Delete(root);
    return err;
}

static esp_err_t api_wifi_post_handler(httpd_req_t *req)
{
    char body[256] = {0};
    if (read_request_body(req, body, sizeof(body)) <= 0) {
        return send_json_error(req, "400 Bad Request", "invalid body");
    }

    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        return send_json_error(req, "400 Bad Request", "invalid json");
    }

    const cJSON *action = cJSON_GetObjectItem(root, "action");
    if (cJSON_IsString(action) && strcmp(action->valuestring, "reprovision") == 0) {
        cJSON_Delete(root);

        cJSON *resp = cJSON_CreateObject();
        cJSON_AddBoolToObject(resp, "ok", true);
        cJSON_AddStringToObject(resp, "message", "正在开启配网热点 MCP-Sensors-Setup…");
        esp_err_t err = send_json(req, resp);
        cJSON_Delete(resp);
        if (err != ESP_OK) {
            return err;
        }

        if (xTaskCreate(wifi_reprovision_task, "wifi_reprov", 4096, NULL, 5, NULL) != pdPASS) {
            return ESP_ERR_NO_MEM;
        }
        return ESP_OK;
    }

    const cJSON *ssid_item = cJSON_GetObjectItem(root, "ssid");
    const cJSON *pass_item = cJSON_GetObjectItem(root, "password");
    if (!cJSON_IsString(ssid_item) || ssid_item->valuestring[0] == '\0') {
        cJSON_Delete(root);
        return send_json_error(req, "400 Bad Request", "missing ssid");
    }

    char ssid_buf[33] = {0};
    char pass_buf[65] = {0};
    strncpy(ssid_buf, ssid_item->valuestring, sizeof(ssid_buf) - 1);
    if (cJSON_IsString(pass_item)) {
        strncpy(pass_buf, pass_item->valuestring, sizeof(pass_buf) - 1);
    }
    ESP_LOGI(TAG, "save WiFi via API: ssid=%s", ssid_buf);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddStringToObject(resp, "message", "正在连接 WiFi...");
    esp_err_t err = send_json(req, resp);
    cJSON_Delete(resp);
    cJSON_Delete(root);

    if (err != ESP_OK) {
        return err;
    }

    wifi_connect_params_t *params = calloc(1, sizeof(*params));
    if (params == NULL) {
        return ESP_ERR_NO_MEM;
    }
    strncpy(params->ssid, ssid_buf, sizeof(params->ssid) - 1);
    strncpy(params->pass, pass_buf, sizeof(params->pass) - 1);

    if (xTaskCreate(wifi_connect_task, "wifi_connect", 4096, params, 5, NULL) != pdPASS) {
        free(params);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t captive_redirect_handler(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    if (s_provision_mode) {
        return captive_portal_redirect(req);
    }
    httpd_resp_send_404(req);
    return ESP_FAIL;
}

static void register_api_handlers(void)
{
    httpd_uri_t data = {.uri = "/api/data", .method = HTTP_GET, .handler = api_data_handler};
    httpd_uri_t cfg_get = {.uri = "/api/config", .method = HTTP_GET, .handler = api_config_get_handler};
    httpd_uri_t cfg_post = {.uri = "/api/config", .method = HTTP_POST, .handler = api_config_post_handler};
    httpd_uri_t coord = {.uri = "/api/coord", .method = HTTP_GET, .handler = api_coord_handler};
    httpd_uri_t wifi_get = {.uri = "/api/wifi", .method = HTTP_GET, .handler = api_wifi_get_handler};
    httpd_uri_t wifi = {.uri = "/api/wifi", .method = HTTP_POST, .handler = api_wifi_post_handler};

    httpd_register_uri_handler(s_server, &data);
    httpd_register_uri_handler(s_server, &cfg_get);
    httpd_register_uri_handler(s_server, &cfg_post);
    httpd_register_uri_handler(s_server, &coord);
    httpd_register_uri_handler(s_server, &wifi_get);
    httpd_register_uri_handler(s_server, &wifi);
}

static esp_err_t start_server(const char *index_page, bool provision_mode)
{
    if (s_server && (strcmp(s_index_page, index_page) != 0 || s_provision_mode != provision_mode)) {
        web_server_stop();
    }

    if (s_server) {
        return ESP_OK;
    }

    s_index_page = index_page;
    s_provision_mode = provision_mode;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 32;
    config.stack_size = 8192;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.lru_purge_enable = true;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    register_api_handlers();

    if (provision_mode) {
        err = web_server_register_static_routes();
        if (err != ESP_OK) {
            return err;
        }
        httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, captive_redirect_handler);
        ESP_LOGI(TAG, "provision server: http://192.168.4.1");
    } else {
        ESP_LOGI(TAG, "dashboard httpd ready (register MCP before static routes)");
    }

    return ESP_OK;
}

httpd_handle_t web_server_get_handle(void)
{
    return s_server;
}

esp_err_t web_server_register_static_routes(void)
{
    if (!s_server) {
        return ESP_ERR_INVALID_STATE;
    }

    httpd_uri_t static_files = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = static_file_handler,
    };
    esp_err_t err = httpd_register_uri_handler(s_server, &static_files);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "static route register failed: %s", esp_err_to_name(err));
        return err;
    }

    if (!s_provision_mode) {
        ESP_LOGI(TAG, "dashboard server started");
    }
    return ESP_OK;
}

esp_err_t web_server_start_provision(void)
{
    return start_server("index.html", true);
}

esp_err_t web_server_start_dashboard(void)
{
    return start_server("index.html", false);
}

void web_server_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
}
