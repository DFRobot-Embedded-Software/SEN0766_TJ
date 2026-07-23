/**
 * @file ltr303_sensor.c
 * @brief LTR-303ALS-01：I2C ALS，输�?lux
 */
#include "ltr303_sensor.h"

#include "esp_log.h"
#include "sdkconfig.h"

#if CONFIG_LTR303_ENABLE
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#endif

static const char *TAG = "ltr303";

#if CONFIG_LTR303_ENABLE

#define LTR303_ADDR_7BIT       0x29
#define LTR303_REG_CONTR       0x80
#define LTR303_REG_MEAS_RATE   0x85
#define LTR303_REG_PART_ID     0x86
#define LTR303_REG_DATA_CH1_0  0x88
#define LTR303_I2C_TIMEOUT_MS  200
#define LTR303_PART_ID_EXPECT  0xA0

static i2c_master_bus_handle_t s_i2c_bus;
static i2c_master_dev_handle_t s_i2c_dev;
static SemaphoreHandle_t s_lock;
static bool s_ready;
static bool s_has_data;
static float s_lux;

static esp_err_t ensure_i2c_bus(void)
{
    if (s_i2c_bus != NULL) {
        return ESP_OK;
    }

    if (i2c_master_get_bus_handle(I2C_NUM_0, &s_i2c_bus) == ESP_OK && s_i2c_bus != NULL) {
        ESP_LOGI(TAG, "reuse I2C0 bus");
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
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
    if (err != ESP_OK) {
        return err;
    }
    ESP_LOGI(TAG, "created I2C0 SDA=%d SCL=%d", CONFIG_SENSOR_I2C0_SDA, CONFIG_SENSOR_I2C0_SCL);
    return ESP_OK;
}

static esp_err_t write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_i2c_dev, buf, sizeof(buf), LTR303_I2C_TIMEOUT_MS);
}

static esp_err_t read_regs(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(s_i2c_dev, &reg, 1, data, len, LTR303_I2C_TIMEOUT_MS);
}

static float calc_lux(uint16_t ch0, uint16_t ch1)
{
    if (ch0 == 0 && ch1 == 0) {
        return 0.0f;
    }
    float ratio = (float)ch1 / ((float)ch0 + (float)ch1);
    float lux;
    if (ratio < 0.45f) {
        lux = 1.7743f * (float)ch0 + 1.1059f * (float)ch1;
    } else if (ratio < 0.64f) {
        lux = 4.2785f * (float)ch0 - 1.9548f * (float)ch1;
    } else if (ratio < 0.85f) {
        lux = 0.5926f * (float)ch0 + 0.1185f * (float)ch1;
    } else {
        lux = 0.0f;
    }
    if (lux < 0.0f) {
        lux = 0.0f;
    }
    return lux;
}

static esp_err_t read_lux_once(float *lux_out)
{
    uint8_t raw[4];
    esp_err_t err = read_regs(LTR303_REG_DATA_CH1_0, raw, sizeof(raw));
    if (err != ESP_OK) {
        return err;
    }
    uint16_t ch1 = (uint16_t)(raw[0] | ((uint16_t)raw[1] << 8));
    uint16_t ch0 = (uint16_t)(raw[2] | ((uint16_t)raw[3] << 8));
    *lux_out = calc_lux(ch0, ch1);
    return ESP_OK;
}

static void poll_task(void *arg)
{
    (void)arg;
    while (true) {
        float lux = 0.0f;
        esp_err_t err = read_lux_once(&lux);
        if (err == ESP_OK) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_lux = lux;
            s_has_data = true;
            xSemaphoreGive(s_lock);
        } else {
            ESP_LOGW(TAG, "read failed: %s", esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(CONFIG_LTR303_POLL_INTERVAL_MS));
    }
}

esp_err_t ltr303_sensor_start(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = ensure_i2c_bus();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus failed: %s", esp_err_to_name(err));
        return err;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = LTR303_ADDR_7BIT,
        .scl_speed_hz = 100000,
    };
    err = i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_i2c_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add device failed: %s", esp_err_to_name(err));
        return err;
    }

    if (i2c_master_probe(s_i2c_bus, LTR303_ADDR_7BIT, 100) != ESP_OK) {
        ESP_LOGE(TAG, "no ACK at 0x%02X", LTR303_ADDR_7BIT);
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t part_id = 0;
    err = read_regs(LTR303_REG_PART_ID, &part_id, 1);
    if (err != ESP_OK || part_id != LTR303_PART_ID_EXPECT) {
        ESP_LOGE(TAG, "PART_ID=0x%02X (expect 0x%02X)", part_id, LTR303_PART_ID_EXPECT);
        return ESP_ERR_INVALID_RESPONSE;
    }

    err = write_reg(LTR303_REG_CONTR, 0x01);
    if (err != ESP_OK) {
        return err;
    }
    err = write_reg(LTR303_REG_MEAS_RATE, 0x03);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(20));

    if (xTaskCreate(poll_task, "ltr303", 3072, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    s_ready = true;
    ESP_LOGI(TAG, "LTR-303 ready addr=0x%02X", LTR303_ADDR_7BIT);
    return ESP_OK;
}

bool ltr303_sensor_is_ready(void)
{
    return s_ready;
}

esp_err_t ltr303_sensor_get(float *lux)
{
    if (lux == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready || !s_has_data) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *lux = s_lux;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

#else /* !CONFIG_LTR303_ENABLE */

esp_err_t ltr303_sensor_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

bool ltr303_sensor_is_ready(void)
{
    return false;
}

esp_err_t ltr303_sensor_get(float *lux)
{
    (void)lux;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
