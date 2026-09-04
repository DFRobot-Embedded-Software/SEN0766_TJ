/**
 * @file device_id.c
 * @brief 优先 WIFI_STA_DEF MAC，失败再熔丝。后 3 字节 → 6 hex。
 */
/**
 * @file device_id.c
 * @brief 优先 WIFI_STA_DEF MAC，失败再熔丝。始终取 mac[3..5]，小写 hex。
 */
#include "device_id.h"

#include <stdio.h>
#include <string.h>

#include "esp_mac.h"
#include "esp_netif.h"

esp_err_t device_id_get(char *out, size_t out_len)
{
    if (!out || out_len < 7) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t mac[6] = {0};
    esp_err_t err = ESP_FAIL;

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        err = esp_netif_get_mac(netif, mac);
    }
    if (err != ESP_OK) {
        err = esp_read_mac(mac, ESP_MAC_EFUSE_FACTORY);
    }
    if (err != ESP_OK) {
        err = esp_read_mac(mac, ESP_MAC_BASE);
    }
    if (err != ESP_OK) {
        return err;
    }
    snprintf(out, out_len, "%02x%02x%02x", mac[3], mac[4], mac[5]);
    return ESP_OK;
}

esp_err_t device_id_hostname(char *out, size_t out_len)
{
    char id[8] = {0};
    esp_err_t err = device_id_get(id, sizeof(id));
    if (err != ESP_OK) {
        return err;
    }
    snprintf(out, out_len, "mcp-sensors-%s", id);
    return ESP_OK;
}
