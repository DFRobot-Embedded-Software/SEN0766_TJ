#include "web_fs.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_spiffs.h"

static const char *TAG = "web_fs";

static const char *content_type_for_path(const char *path)
{
    const char *ext = strrchr(path, '.');
    if (ext == NULL) {
        return "application/octet-stream";
    }
    if (strcmp(ext, ".html") == 0) {
        return "text/html; charset=utf-8";
    }
    if (strcmp(ext, ".css") == 0) {
        return "text/css; charset=utf-8";
    }
    if (strcmp(ext, ".js") == 0) {
        return "application/javascript; charset=utf-8";
    }
    if (strcmp(ext, ".json") == 0) {
        return "application/json";
    }
    if (strcmp(ext, ".png") == 0) {
        return "image/png";
    }
    if (strcmp(ext, ".ico") == 0) {
        return "image/x-icon";
    }
    if (strcmp(ext, ".svg") == 0) {
        return "image/svg+xml";
    }
    return "application/octet-stream";
}

esp_err_t web_fs_init(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = WEB_FS_MOUNT_POINT,
        .partition_label = "storage",
        .max_files = 32,
        .format_if_mount_failed = true,
    };

    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(err));
        return err;
    }

    size_t total = 0;
    size_t used = 0;
    err = esp_spiffs_info(conf.partition_label, &total, &used);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS: total=%u, used=%u", (unsigned)total, (unsigned)used);
    }
    return ESP_OK;
}

esp_err_t web_fs_send_file(httpd_req_t *req, const char *filepath)
{
    FILE *file = fopen(filepath, "rb");
    if (file == NULL) {
        ESP_LOGW(TAG, "file not found: %s", filepath);
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, content_type_for_path(filepath));

    char buffer[1024];
    size_t read_bytes;
    while ((read_bytes = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        if (httpd_resp_send_chunk(req, buffer, read_bytes) != ESP_OK) {
            fclose(file);
            httpd_resp_send_chunk(req, NULL, 0);
            return ESP_FAIL;
        }
    }

    fclose(file);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}
