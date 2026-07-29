#include "ota_update.h"

#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_spiffs.h"

static const char *TAG = "ota_update";

static esp_ota_handle_t s_handle;
static const esp_partition_t *s_update_partition;
static bool s_active;

static const esp_partition_t *s_spiffs_part;
static size_t s_spiffs_written;
static size_t s_spiffs_expected;
static bool s_spiffs_active;

static esp_err_t remount_storage(bool format_if_failed)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "storage",
        .max_files = 32,
        .format_if_mount_failed = format_if_failed,
    };
    return esp_vfs_spiffs_register(&conf);
}

esp_err_t ota_update_begin(void)
{
    if (s_active || s_spiffs_active) {
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
    ESP_LOGI(TAG, "App OTA begin -> %s", s_update_partition->label);
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
    ESP_LOGW(TAG, "App OTA aborted");
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

    ESP_LOGI(TAG, "App OTA committed to %s, waiting for reboot", s_update_partition->label);
    return ESP_OK;
}

esp_err_t ota_update_spiffs_begin(size_t image_size)
{
    if (s_active || s_spiffs_active) {
        return ESP_ERR_INVALID_STATE;
    }

    s_spiffs_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                             ESP_PARTITION_SUBTYPE_DATA_SPIFFS,
                                             "storage");
    if (s_spiffs_part == NULL) {
        ESP_LOGE(TAG, "storage SPIFFS partition not found");
        return ESP_ERR_NOT_FOUND;
    }
    if (image_size == 0 || image_size > s_spiffs_part->size) {
        ESP_LOGE(TAG, "UI image size %u invalid (partition %u)",
                 (unsigned)image_size, (unsigned)s_spiffs_part->size);
        return ESP_ERR_INVALID_SIZE;
    }

    if (esp_spiffs_mounted("storage")) {
        esp_err_t um = esp_vfs_spiffs_unregister("storage");
        if (um != ESP_OK) {
            ESP_LOGE(TAG, "SPIFFS unmount failed: %s", esp_err_to_name(um));
            return um;
        }
    }

    esp_err_t err = esp_partition_erase_range(s_spiffs_part, 0, s_spiffs_part->size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "erase storage failed: %s", esp_err_to_name(err));
        (void)remount_storage(true);
        return err;
    }

    s_spiffs_written = 0;
    s_spiffs_expected = image_size;
    s_spiffs_active = true;
    ESP_LOGI(TAG, "UI OTA begin -> storage (%u / %u bytes)",
             (unsigned)image_size, (unsigned)s_spiffs_part->size);
    return ESP_OK;
}

esp_err_t ota_update_spiffs_write(const uint8_t *data, size_t len)
{
    if (!s_spiffs_active || s_spiffs_part == NULL || data == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_spiffs_written + len > s_spiffs_expected) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = esp_partition_write(s_spiffs_part, s_spiffs_written, data, len);
    if (err != ESP_OK) {
        return err;
    }
    s_spiffs_written += len;
    return ESP_OK;
}

void ota_update_spiffs_abort(void)
{
    if (!s_spiffs_active) {
        return;
    }
    s_spiffs_active = false;
    ESP_LOGW(TAG, "UI OTA aborted after %u bytes — SPIFFS may be corrupt",
             (unsigned)s_spiffs_written);
    esp_err_t err = remount_storage(true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "remount after abort failed: %s", esp_err_to_name(err));
    }
}

esp_err_t ota_update_spiffs_end(void)
{
    if (!s_spiffs_active) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_spiffs_written != s_spiffs_expected) {
        ESP_LOGE(TAG, "UI OTA incomplete: %u != %u",
                 (unsigned)s_spiffs_written, (unsigned)s_spiffs_expected);
        ota_update_spiffs_abort();
        return ESP_ERR_INVALID_SIZE;
    }

    s_spiffs_active = false;
    esp_err_t err = remount_storage(false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "remount storage failed: %s — try format", esp_err_to_name(err));
        err = remount_storage(true);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "storage remount failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "UI OTA committed (%u bytes)", (unsigned)s_spiffs_written);
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

    const esp_partition_t *storage = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                              ESP_PARTITION_SUBTYPE_DATA_SPIFFS,
                                                              "storage");
    if (storage != NULL) {
        out->storage_size = storage->size;
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
