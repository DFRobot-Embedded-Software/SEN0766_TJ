/**
 * @file mdns_hub.c
 * @brief 实现 mdns_hub.h。改服务类型或 TXT 键必须同步：
 *        HA manifest.json zeroconf、mcp_gw/mdns_devices.py SERVICE。
 */
#include "mdns_hub.h"

#include "device_id.h"
#include "esp_log.h"
#include "mdns.h"
#include "sdkconfig.h"

static const char *TAG = "mdns_hub";
static bool s_started;

esp_err_t mdns_hub_start(void)
{
    char id[8] = {0};
    char host[32] = {0};
    char inst[40] = {0};
    if (device_id_get(id, sizeof(id)) != ESP_OK) {
        return ESP_FAIL;
    }
    snprintf(host, sizeof(host), "mcp-sensors-%s", id);
    snprintf(inst, sizeof(inst), "MCP Sensors %s", id);

    mdns_hub_stop();
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        return err;
    }
    mdns_hostname_set(host);
    mdns_instance_name_set(inst);
    /* 网页用；业务发现不要用 _http 代替 _mcp-sensors */
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);

    static char id_txt[8];
    static char ver_txt[8];
    static char name_txt[40];
    strncpy(id_txt, id, sizeof(id_txt) - 1);
    strncpy(ver_txt, "1", sizeof(ver_txt) - 1);
    strncpy(name_txt, inst, sizeof(name_txt) - 1);
    mdns_txt_item_t txt[] = {
        {.key = "id", .value = id_txt},
        {.key = "ver", .value = ver_txt},
        {.key = "name", .value = name_txt},
    };
    /* TXT 只放身份。温度/占用进 CoAP snapshot，进门进 MQTT event。 */
    err = mdns_service_add(NULL, "_mcp-sensors", "_udp", 5683, txt, 3);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "_mcp-sensors advertise failed: %s", esp_err_to_name(err));
    }
    s_started = true;
    ESP_LOGI(TAG, "advertised %s.local _http:80 _mcp-sensors:5683 id=%s", host, id);
    return ESP_OK;
}

void mdns_hub_stop(void)
{
    if (s_started) {
        mdns_free();
        s_started = false;
    }
}
