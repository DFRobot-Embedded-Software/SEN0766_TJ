#include "board_snapshot.h"
#include "env_sensors.h"
#include "sensor_settings.h"

#include <string.h>

void board_snapshot_init(void)
{
    env_sensors_init();
    radar_area_init();
    sensor_settings_init();
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
