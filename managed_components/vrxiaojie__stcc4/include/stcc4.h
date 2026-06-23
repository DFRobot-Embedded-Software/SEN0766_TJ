#pragma once

#include "esp_log.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define STCC4_I2C_ADDR_64 0x64
#define STCC4_I2C_ADDR_65 0x65

typedef struct
{
    uint8_t init;
    int16_t co2Concentration;
    float temperature;
    float relativeHumidity;
    uint16_t sensorStatus;
    uint32_t productId;
    uint64_t serialNumber;
} STCC4_t;

esp_err_t stcc4_i2c_init(i2c_master_bus_handle_t bus_handle);
esp_err_t stcc4_i2c_deinit(i2c_master_bus_handle_t bus_handle);

esp_err_t stcc4_start_continuous_measurement();
esp_err_t stcc4_stop_continuous_measurement();
esp_err_t stcc4_read_measurement_raw(int16_t *co2ConcentrationRaw, uint16_t *temperatureRaw,
                                     uint16_t *relativeHumidityRaw, uint16_t *sensorStatusRaw);
esp_err_t stcc4_read_measurement(int16_t *co2Concentration, float *temperature,
                                 float *relativeHumidity, uint16_t *sensorStatus);
esp_err_t stcc4_set_pressure_compensation(uint16_t pressure);
esp_err_t stcc4_set_rht_compensation(uint16_t rawTemperature, uint16_t rawHumidity);
esp_err_t stcc4_measure_single_shot();
esp_err_t stcc4_enter_sleep_mode();
esp_err_t stcc4_exit_sleep_mode();
esp_err_t stcc4_perform_conditioning();
esp_err_t stcc4_perform_factory_reset(uint16_t *factoryResetResult);
esp_err_t stcc4_perform_self_test(uint16_t *testResult);
esp_err_t stcc4_enable_testing_mode();
esp_err_t stcc4_disable_testing_mode();
esp_err_t stcc4_perform_forced_recalibration(int16_t targetCO2Concentration, int16_t *frcCorrection);
esp_err_t stcc4_get_product_id(uint32_t *productId, uint64_t *serialNumber);
