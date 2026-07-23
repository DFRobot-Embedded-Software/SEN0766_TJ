#include "stcc4_sensor.h"

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "stcc4";

#define STCC4_I2C_ADDR_7BIT 0x64
#define STCC4_I2C_TIMEOUT_MS 1000

static i2c_master_bus_handle_t s_i2c_bus;
static i2c_master_dev_handle_t s_i2c_dev;
static SemaphoreHandle_t s_lock;
static bool s_ready;
static bool s_has_data;
static float s_temperature;
static float s_humidity;
static int s_co2;

static uint8_t stcc4_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0xFF;
    const uint8_t poly = 0x31;

    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            if (crc & 0x80) {
                crc = (uint8_t)((crc << 1) ^ poly);
            } else {
                crc = (uint8_t)(crc << 1);
            }
        }
    }
    return crc;
}

static esp_err_t stcc4_tx(const uint8_t *buf, size_t len)
{
    if (s_i2c_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = i2c_master_transmit(s_i2c_dev, buf, len, STCC4_I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "I2C tx failed: %s", esp_err_to_name(err));
        (void)i2c_master_bus_reset(s_i2c_bus);
    }
    return err;
}

static esp_err_t stcc4_tx_cmd(uint8_t cmd_hi, uint8_t cmd_lo)
{
    const uint8_t buf[2] = {cmd_hi, cmd_lo};
    return stcc4_tx(buf, sizeof(buf));
}

static esp_err_t stcc4_rx(uint8_t *buf, size_t len)
{
    if (s_i2c_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = i2c_master_receive(s_i2c_dev, buf, len, STCC4_I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "I2C rx failed: %s", esp_err_to_name(err));
        (void)i2c_master_bus_reset(s_i2c_bus);
    }
    return err;
}

static esp_err_t init_i2c_bus(void)
{
    if (s_i2c_bus != NULL) {
        return ESP_OK;
    }

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = CONFIG_SENSOR_I2C0_SDA,
        .scl_io_num = CONFIG_SENSOR_I2C0_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    return i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
}

static esp_err_t init_i2c_device(void)
{
    if (s_i2c_dev != NULL) {
        return ESP_OK;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = STCC4_I2C_ADDR_7BIT,
        .scl_speed_hz = 100000,
    };

    esp_err_t err = i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_i2c_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add STCC4 device failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "STCC4 I2C device ready (addr=0x%02X, 100kHz)", STCC4_I2C_ADDR_7BIT);
    return ESP_OK;
}

static esp_err_t stcc4_stop_continuous(void)
{
    esp_err_t err = stcc4_tx_cmd(0x3f, 0x86);
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(1200));
    }
    return err;
}

static esp_err_t stcc4_start_continuous(void)
{
    for (int attempt = 0; attempt < 3; attempt++) {
        if (attempt > 0) {
            (void)i2c_master_bus_reset(s_i2c_bus);
            vTaskDelay(pdMS_TO_TICKS(20));
        }

        esp_err_t err = stcc4_tx_cmd(0x21, 0x8b);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "STCC4 continuous measurement started");
            return ESP_OK;
        }

        ESP_LOGW(TAG, "start continuous failed (attempt %d): %s",
                 attempt + 1, esp_err_to_name(err));
    }
    return ESP_FAIL;
}

static float raw_to_temperature(uint16_t raw)
{
    return -45.0f + ((175.0f * (float)raw) / 65535.0f);
}

static float raw_to_humidity(uint16_t raw)
{
    return -6.0f + ((125.0f * (float)raw) / 65535.0f);
}

static esp_err_t stcc4_read_raw(int16_t *co2, uint16_t *temp_raw, uint16_t *rh_raw, uint16_t *status)
{
    esp_err_t err = stcc4_tx_cmd(0xEC, 0x05);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(1));

    uint8_t rx[12] = {0};
    err = stcc4_rx(rx, sizeof(rx));
    if (err != ESP_OK) {
        return err;
    }

    if (stcc4_crc8(&rx[0], 2) != rx[2]) {
        return ESP_ERR_INVALID_CRC;
    }
    if (stcc4_crc8(&rx[3], 2) != rx[5]) {
        return ESP_ERR_INVALID_CRC;
    }
    if (stcc4_crc8(&rx[6], 2) != rx[8]) {
        return ESP_ERR_INVALID_CRC;
    }
    if (stcc4_crc8(&rx[9], 2) != rx[11]) {
        return ESP_ERR_INVALID_CRC;
    }

    *co2 = (int16_t)((rx[0] << 8) | rx[1]);
    *temp_raw = (uint16_t)((rx[3] << 8) | rx[4]);
    *rh_raw = (uint16_t)((rx[6] << 8) | rx[7]);
    *status = (uint16_t)((rx[9] << 8) | rx[10]);
    return ESP_OK;
}

static esp_err_t stcc4_read_safe(int16_t *co2, float *temp, float *rh, uint16_t *status)
{
    uint16_t raw_temp = 0;
    uint16_t raw_rh = 0;
    esp_err_t err = stcc4_read_raw(co2, &raw_temp, &raw_rh, status);
    if (err != ESP_OK) {
        return err;
    }

    *temp = raw_to_temperature(raw_temp);
    *rh = raw_to_humidity(raw_rh);
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
    if (i2c_master_probe(s_i2c_bus, addr, STCC4_I2C_TIMEOUT_MS) != ESP_OK) {
        ESP_LOGE(TAG, "STCC4 not found at 0x%02X (SDA=%d SCL=%d)",
                 addr, CONFIG_SENSOR_I2C0_SDA, CONFIG_SENSOR_I2C0_SCL);
        return ESP_ERR_NOT_FOUND;
    }

    if (addr != STCC4_I2C_ADDR_7BIT) {
        ESP_LOGW(TAG, "menuconfig addr 0x%02X != 0x64; driver uses 0x64", addr);
    }

    err = init_i2c_device();
    if (err != ESP_OK) {
        return err;
    }

    /* 若上次未正常 stop，先停再启，避免 start 命令 NACK */
    (void)stcc4_stop_continuous();

    err = stcc4_start_continuous();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start continuous measurement failed: %s", esp_err_to_name(err));
        return err;
    }

    s_ready = true;
    ESP_LOGI(TAG, "STCC4 started (SDA=%d SCL=%d, interval=%dms)",
             CONFIG_SENSOR_I2C0_SDA, CONFIG_SENSOR_I2C0_SCL, CONFIG_STCC4_POLL_INTERVAL_MS);

    BaseType_t ok = xTaskCreate(stcc4_poll_task, "stcc4", 4096, NULL, 5, NULL);
    if (ok != pdPASS) {
        s_ready = false;
        (void)stcc4_stop_continuous();
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
