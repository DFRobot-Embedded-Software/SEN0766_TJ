/**
 * @file mcp_core.c
 * @brief MCP HTTP 传输 + Provider 注册表
 */
#include "mcp_core.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mcp_mgr.h"
#include "sdkconfig.h"

static const char *TAG = "mcp_core";

static esp_mcp_t *s_mcp;
static esp_mcp_mgr_handle_t s_mgr;
static bool s_running;

static const mcp_sensor_provider_t *s_providers[MCP_CORE_MAX_PROVIDERS];
static int s_provider_count;

typedef struct {
    esp_mcp_mgr_handle_t mgr;
    httpd_handle_t httpd;
} mcp_sdk_http_view_t;

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

esp_err_t mcp_core_add_provider(const mcp_sensor_provider_t *provider)
{
    if (provider == NULL || provider->name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_running) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_provider_count >= MCP_CORE_MAX_PROVIDERS) {
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < s_provider_count; i++) {
        if (s_providers[i] == provider) {
            return ESP_OK;
        }
    }
    s_providers[s_provider_count++] = provider;
    ESP_LOGI(TAG, "provider registered: %s", provider->name);
    return ESP_OK;
}

esp_mcp_t *mcp_core_get_mcp(void)
{
    return s_mcp;
}

void mcp_core_notify_resource(const char *uri)
{
    if (!s_running || s_mgr == 0 || uri == NULL) {
        return;
    }

    char msg[160];
    int n = snprintf(msg, sizeof(msg),
                     "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/resources/updated\","
                     "\"params\":{\"uri\":\"%s\"}}",
                     uri);
    if (n <= 0 || (size_t)n >= sizeof(msg)) {
        ESP_LOGW(TAG, "notify truncated: %s", uri);
        return;
    }

    esp_err_t err = esp_mcp_mgr_emit_message(s_mgr, NULL, msg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "notify failed (%s): %s", uri, esp_err_to_name(err));
    }
}

esp_err_t mcp_core_start(void)
{
    if (s_running) {
        return ESP_OK;
    }
    if (s_provider_count <= 0) {
        ESP_LOGW(TAG, "no providers registered");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = esp_mcp_create(&s_mcp);
    ESP_RETURN_ON_ERROR(err, TAG, "esp_mcp_create failed");

    for (int i = 0; i < s_provider_count; i++) {
        const mcp_sensor_provider_t *p = s_providers[i];
        if (p->register_tools != NULL) {
            err = p->register_tools(s_mcp);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "provider %s register_tools: %s", p->name, esp_err_to_name(err));
                goto fail_mcp;
            }
        }
        if (p->register_resources != NULL) {
            err = p->register_resources(s_mcp);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "provider %s register_resources: %s", p->name, esp_err_to_name(err));
                goto fail_mcp;
            }
        }
    }

    ESP_ERROR_CHECK(esp_mcp_set_server_info(
        s_mcp,
        "MCP Sensors",
        "ESP32-P4 sensor hub (provider-based MCP).",
        NULL,
        NULL));
    ESP_ERROR_CHECK(esp_mcp_set_instructions(
        s_mcp,
        "Use sensors.get_snapshot for live readings. "
        "Subscribe to sensor://temperature for environment push updates. "
        "Subscribe to sensor://events for radar events; areas[] follows Web config. "
        "Per-area: sensor://area/{id}/events. "
        "Event types: room_empty, area_enter, area_exit, area_dwell. "
        "Read after notifications/resources/updated."));

    httpd_config_t http_config = HTTPD_DEFAULT_CONFIG();
    http_config.server_port = MCP_HTTP_PORT;
    http_config.ctrl_port += 1;
    http_config.stack_size = 8192;
    http_config.max_uri_handlers = 48;
    http_config.max_open_sockets = 7;
    http_config.lru_purge_enable = true;

    ESP_LOGI(TAG, "Starting MCP HTTP on port %d (ctrl_port %d)",
             http_config.server_port, http_config.ctrl_port);

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
        goto fail_mcp;
    }

    err = esp_mcp_mgr_start(s_mgr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_mcp_mgr_start failed: %s", esp_err_to_name(err));
        goto fail_mgr;
    }

    err = esp_mcp_mgr_register_endpoint(s_mgr, MCP_ENDPOINT_NAME, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register endpoint failed: %s", esp_err_to_name(err));
        esp_mcp_mgr_stop(s_mgr);
        goto fail_mgr;
    }

    s_running = true;

    for (int i = 0; i < s_provider_count; i++) {
        const mcp_sensor_provider_t *p = s_providers[i];
        if (p->start_runtime == NULL) {
            continue;
        }
        err = p->start_runtime();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "provider %s start_runtime: %s", p->name, esp_err_to_name(err));
        }
    }

    ESP_LOGI(TAG, "MCP ready http://<ip>:%d%s (%d providers)",
             MCP_HTTP_PORT, MCP_ENDPOINT_URI, s_provider_count);
    return ESP_OK;

fail_mgr:
    esp_mcp_mgr_deinit(s_mgr);
    s_mgr = 0;
fail_mcp:
    esp_mcp_destroy(s_mcp);
    s_mcp = NULL;
    return err;
}

void mcp_core_stop(void)
{
    if (!s_running) {
        return;
    }

    s_running = false;

    for (int i = s_provider_count - 1; i >= 0; i--) {
        const mcp_sensor_provider_t *p = s_providers[i];
        if (p->stop_runtime != NULL) {
            p->stop_runtime();
        }
    }

    if (s_mgr != 0) {
        esp_mcp_mgr_unregister_endpoint(s_mgr, MCP_ENDPOINT_NAME);
        esp_mcp_mgr_stop(s_mgr);
        esp_mcp_mgr_deinit(s_mgr);
        s_mgr = 0;
    }

    if (s_mcp != NULL) {
        esp_mcp_destroy(s_mcp);
        s_mcp = NULL;
    }

    ESP_LOGI(TAG, "MCP core stopped");
}

bool mcp_core_is_running(void)
{
    return s_running;
}
