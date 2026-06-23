#include "stcc4_sensor.h"

#include "stcc4.h"

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "stcc4";

static i2c_master_bus_handle_t s_i2c_bus;
static SemaphoreHandle_t s_lock;
static bool s_ready;
static bool s_has_data;
static float s_temperature;
static float s_humidity;
static int s_co2;

static esp_err_t init_i2c_bus(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = CONFIG_STCC4_I2C_SDA,
        .scl_io_num = CONFIG_STCC4_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    return i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
}

static float raw_to_temperature(uint16_t raw)
{
    return -45.0f + ((175.0f * (float)raw) / 65535.0f);
}

static float raw_to_humidity(uint16_t raw)
{
    return -6.0f + ((125.0f * (float)raw) / 65535.0f);
}

/** 安全读数：驱动 stcc4_read_measurement() 在 CRC 失败时会 ESP_ERROR_CHECK 崩溃 */
static esp_err_t stcc4_read_safe(int16_t *co2, float *temp, float *rh, uint16_t *status)
{
    int16_t raw_co2 = 0;
    uint16_t raw_temp = 0;
    uint16_t raw_rh = 0;
    uint16_t raw_status = 0;

    esp_err_t err = stcc4_read_measurement_raw(&raw_co2, &raw_temp, &raw_rh, &raw_status);
    if (err != ESP_OK) {
        return err;
    }

    *co2 = raw_co2;
    *temp = raw_to_temperature(raw_temp);
    *rh = raw_to_humidity(raw_rh);
    *status = raw_status;
    return ESP_OK;
}

static void stcc4_poll_task(void *arg)
{
    (void)arg;
    const TickType_t steady_interval = pdMS_TO_TICKS(CONFIG_STCC4_POLL_INTERVAL_MS);
    const TickType_t warmup_interval = pdMS_TO_TICKS(500);

    while (1) {
        int16_t co2 = 0;
        float temp = 0.0f;
        float rh = 0.0f;
        uint16_t status = 0;

        esp_err_t err = stcc4_read_safe(&co2, &temp, &rh, &status);
        if (err == ESP_OK) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_co2 = co2;
            s_temperature = temp;
            s_humidity = rh;
            s_has_data = true;
            xSemaphoreGive(s_lock);

            //ESP_LOGI(TAG, "CO2=%d ppm, T=%.1f°C, RH=%.1f%%, status=0x%04X",
            //         (int)co2, temp, rh, status);
        } else if (s_has_data) {
            ESP_LOGW(TAG, "read failed: %s", esp_err_to_name(err));
        }

        vTaskDelay(s_has_data ? steady_interval : warmup_interval);
    }
}

esp_err_t stcc4_sensor_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = init_i2c_bus();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed: %s", esp_err_to_name(err));
        return err;
    }

    uint8_t addr = (uint8_t)CONFIG_STCC4_I2C_ADDR;
    if (i2c_master_probe(s_i2c_bus, addr, 1000) != ESP_OK) {
        ESP_LOGE(TAG, "STCC4 not found at 0x%02X (SDA=%d SCL=%d)",
                 addr, CONFIG_STCC4_I2C_SDA, CONFIG_STCC4_I2C_SCL);
        return ESP_ERR_NOT_FOUND;
    }

    if (addr != STCC4_I2C_ADDR_64) {
        ESP_LOGW(TAG, "driver uses fixed address 0x64; set module jumper to 0x64");
    }

    err = stcc4_i2c_init(s_i2c_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "stcc4_i2c_init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = stcc4_start_continuous_measurement();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start continuous measurement failed: %s", esp_err_to_name(err));
        return err;
    }

    s_ready = true;
    ESP_LOGI(TAG, "STCC4 started (SDA=%d SCL=%d, interval=%dms)",
             CONFIG_STCC4_I2C_SDA, CONFIG_STCC4_I2C_SCL, CONFIG_STCC4_POLL_INTERVAL_MS);

    BaseType_t ok = xTaskCreate(stcc4_poll_task, "stcc4", 4096, NULL, 5, NULL);
    if (ok != pdPASS) {
        s_ready = false;
        stcc4_stop_continuous_measurement();
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

bool stcc4_sensor_is_ready(void)
{
    return s_ready;
}

esp_err_t stcc4_sensor_get(float *temperature, float *humidity, int *co2_ppm)
{
    if (!s_ready) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!s_has_data) {
        return ESP_ERR_INVALID_STATE;
    }
    if (temperature == NULL || humidity == NULL || co2_ppm == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    *temperature = s_temperature;
    *humidity = s_humidity;
    *co2_ppm = s_co2;
    xSemaphoreGive(s_lock);

    return ESP_OK;
}
