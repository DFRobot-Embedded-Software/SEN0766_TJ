#include "board_snapshot.h"
#include "device_event_bus.h"
#include "env_event_detector.h"
#include "env_sensors.h"
#include "radar_events.h"
#include "sensor_settings.h"

#include "esp_log.h"

#include <string.h>

static const char *TAG = "board_snapshot";

void board_snapshot_init(void)
{
    env_sensors_init();
    radar_area_init();
    sensor_settings_init();
    (void)device_event_bus_init();
    (void)env_event_detector_start();
    /* 与 MCP 解耦：Web 仪表盘也需要 range_enter/exit */
    esp_err_t err = radar_events_start(NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "radar_events_start FAILED: %s", esp_err_to_name(err));
    } else {
        ESP_LOGE(TAG, "radar_events_start OK");
    }
}

esp_err_t board_snapshot_get(sensor_snapshot_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));

    env_snapshot_t env = {0};
    env_sensors_get(&env);
    out->env_ready = env.env_ready;
    out->temperature = env.temperature;
    out->humidity = env.humidity;
    out->co2 = env.co2;
    out->lux_ready = env.lux_ready;
    out->lux = env.lux;

    radar_area_get_status(out->areas, &out->area_count);

    return ESP_OK;
}
