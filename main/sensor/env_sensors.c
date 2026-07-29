#include "env_sensors.h"
#include "stcc4_sensor.h"
#include "ltr303_sensor.h"
#include "sc7a20h_sensor.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "env_sensors";

void env_sensors_init(void)
{
    esp_err_t err = stcc4_sensor_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "STCC4 init failed: %s", esp_err_to_name(err));
    }

    err = ltr303_sensor_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "LTR-303 init failed: %s", esp_err_to_name(err));
    }

    err = sc7a20h_sensor_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SC7A20H init failed: %s", esp_err_to_name(err));
    }
}

esp_err_t env_sensors_get(env_snapshot_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));

    if (stcc4_sensor_get(&out->temperature, &out->humidity, &out->co2) == ESP_OK) {
        out->env_ready = true;
    }

    if (ltr303_sensor_get(&out->lux) == ESP_OK) {
        out->lux_ready = true;
    }

    return ESP_OK;
}
