#include "ota_update.h"

#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"

static const char *TAG = "ota_update";

static esp_ota_handle_t s_handle;
static const esp_partition_t *s_update_partition;
static bool s_active;

esp_err_t ota_update_begin(void)
{
    if (s_active) {
        return ESP_ERR_INVALID_STATE;
    }

    s_update_partition = esp_ota_get_next_update_partition(NULL);
    if (s_update_partition == NULL) {
        ESP_LOGE(TAG, "no OTA partition available (check partition table)");
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t err = esp_ota_begin(s_update_partition, OTA_SIZE_UNKNOWN, &s_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        return err;
    }

    s_active = true;
    ESP_LOGI(TAG, "OTA begin -> %s", s_update_partition->label);
    return ESP_OK;
}

esp_err_t ota_update_write(const uint8_t *data, size_t len)
{
    if (!s_active) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_ota_write(s_handle, data, len);
}

void ota_update_abort(void)
{
    if (!s_active) {
        return;
    }
    esp_ota_abort(s_handle);
    s_active = false;
    ESP_LOGW(TAG, "OTA aborted");
}

esp_err_t ota_update_end(void)
{
    if (!s_active) {
        return ESP_ERR_INVALID_STATE;
    }
    s_active = false;

    esp_err_t err = esp_ota_end(s_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed (image likely invalid): %s", esp_err_to_name(err));
        return err;
    }

    err = esp_ota_set_boot_partition(s_update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "OTA committed to %s, waiting for reboot", s_update_partition->label);
    return ESP_OK;
}

esp_err_t ota_update_get_info(ota_info_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running != NULL) {
        strncpy(out->running_label, running->label, sizeof(out->running_label) - 1);
    }

    const esp_app_desc_t *desc = esp_app_get_description();
    if (desc != NULL) {
        strncpy(out->version, desc->version, sizeof(out->version) - 1);
    }

    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (running != NULL && esp_ota_get_state_partition(running, &state) == ESP_OK) {
        out->pending_verify = (state == ESP_OTA_IMG_PENDING_VERIFY);
    }
    return ESP_OK;
}

void ota_update_mark_valid(void)
{
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "mark_app_valid_cancel_rollback: %s", esp_err_to_name(err));
    }
}
