/**
 * @file sc7a20h_sensor.c
 * @brief SC7A20H：I2C 加速度计（LIS2DH 兼容），输出 ax/ay/az (g)
 */
#include "sc7a20h_sensor.h"

#include "esp_log.h"
#include "sdkconfig.h"

#if CONFIG_SC7A20H_ENABLE
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#endif

static const char *TAG = "sc7a20h";

#if CONFIG_SC7A20H_ENABLE

#define SC7A20H_REG_WHO_AM_I  0x0F
#define SC7A20H_REG_CTRL1     0x20
#define SC7A20H_REG_CTRL4     0x23
#define SC7A20H_REG_OUT_X_L   0x28
#define SC7A20H_WHO_AM_I_VAL  0x11
#define SC7A20H_I2C_TIMEOUT_MS 200
#define SC7A20H_MG_PER_LSB    1.0f

static i2c_master_bus_handle_t s_i2c_bus;
static i2c_master_dev_handle_t s_i2c_dev;
static SemaphoreHandle_t s_lock;
static bool s_ready;
static bool s_has_data;
static float s_ax;
static float s_ay;
static float s_az;

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
    return i2c_master_transmit(s_i2c_dev, buf, sizeof(buf), SC7A20H_I2C_TIMEOUT_MS);
}

static esp_err_t read_regs(uint8_t reg, uint8_t *data, size_t len)
{
    uint8_t reg_ai = (uint8_t)(reg | 0x80);
    return i2c_master_transmit_receive(s_i2c_dev, &reg_ai, 1, data, len, SC7A20H_I2C_TIMEOUT_MS);
}

static esp_err_t read_accel_once(float *ax, float *ay, float *az)
{
    uint8_t raw[6];
    esp_err_t err = read_regs(SC7A20H_REG_OUT_X_L, raw, sizeof(raw));
    if (err != ESP_OK) {
        return err;
    }

    int16_t x = (int16_t)((uint16_t)raw[0] | ((uint16_t)raw[1] << 8));
    int16_t y = (int16_t)((uint16_t)raw[2] | ((uint16_t)raw[3] << 8));
    int16_t z = (int16_t)((uint16_t)raw[4] | ((uint16_t)raw[5] << 8));

    *ax = ((float)(x >> 4) * SC7A20H_MG_PER_LSB) / 1000.0f;
    *ay = ((float)(y >> 4) * SC7A20H_MG_PER_LSB) / 1000.0f;
    *az = ((float)(z >> 4) * SC7A20H_MG_PER_LSB) / 1000.0f;
    return ESP_OK;
}

static void poll_task(void *arg)
{
    (void)arg;
    while (true) {
        float ax = 0.0f, ay = 0.0f, az = 0.0f;
        esp_err_t err = read_accel_once(&ax, &ay, &az);
        if (err == ESP_OK) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_ax = ax;
            s_ay = ay;
            s_az = az;
            s_has_data = true;
            xSemaphoreGive(s_lock);
        } else {
            ESP_LOGW(TAG, "read failed: %s", esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(CONFIG_SC7A20H_POLL_INTERVAL_MS));
    }
}

static esp_err_t probe_and_add_device(uint8_t addr_7bit)
{
    if (i2c_master_probe(s_i2c_bus, addr_7bit, 100) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr_7bit,
        .scl_speed_hz = 100000,
    };
    esp_err_t err = i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_i2c_dev);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t who = 0;
    uint8_t reg = SC7A20H_REG_WHO_AM_I;
    err = i2c_master_transmit_receive(s_i2c_dev, &reg, 1, &who, 1, SC7A20H_I2C_TIMEOUT_MS);
    if (err != ESP_OK || who != SC7A20H_WHO_AM_I_VAL) {
        ESP_LOGW(TAG, "addr 0x%02X WHO_AM_I=0x%02X", addr_7bit, who);
        i2c_master_bus_rm_device(s_i2c_dev);
        s_i2c_dev = NULL;
        return ESP_ERR_INVALID_RESPONSE;
    }

    ESP_LOGI(TAG, "found SC7A20H at 0x%02X WHO=0x%02X", addr_7bit, who);
    return ESP_OK;
}

esp_err_t sc7a20h_sensor_start(void)
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

    const uint8_t candidates[] = {
        (uint8_t)CONFIG_SC7A20H_I2C_ADDR,
        0x19,
        0x18,
    };
    err = ESP_ERR_NOT_FOUND;
    for (size_t i = 0; i < sizeof(candidates); i++) {
        bool seen = false;
        for (size_t j = 0; j < i; j++) {
            if (candidates[j] == candidates[i]) {
                seen = true;
                break;
            }
        }
        if (seen) {
            continue;
        }
        err = probe_and_add_device(candidates[i]);
        if (err == ESP_OK) {
            break;
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SC7A20H not found on I2C0");
        return err;
    }

    err = write_reg(SC7A20H_REG_CTRL1, 0x57);
    if (err != ESP_OK) {
        return err;
    }
    err = write_reg(SC7A20H_REG_CTRL4, 0x88);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(20));

    if (xTaskCreate(poll_task, "sc7a20h", 3072, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    s_ready = true;
    ESP_LOGI(TAG, "SC7A20H ready");
    return ESP_OK;
}

bool sc7a20h_sensor_is_ready(void)
{
    return s_ready;
}

esp_err_t sc7a20h_sensor_get(float *ax, float *ay, float *az)
{
    if (ax == NULL || ay == NULL || az == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready || !s_has_data) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *ax = s_ax;
    *ay = s_ay;
    *az = s_az;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

#else /* !CONFIG_SC7A20H_ENABLE */

esp_err_t sc7a20h_sensor_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

bool sc7a20h_sensor_is_ready(void)
{
    return false;
}

esp_err_t sc7a20h_sensor_get(float *ax, float *ay, float *az)
{
    (void)ax;
    (void)ay;
    (void)az;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
