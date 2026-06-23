# vrxiaojie/stcc4
Driver for Sensirion STCC4 CO2 sensor.

## Installation

```sh
idf.py add-dependency vrxiaojie/stcc4
```

## Example
```c
#include "stcc4.h"

#define I2C_MASTER_NUM I2C_NUM_0
#define I2C_MASTER_SDA 3
#define I2C_MASTER_SCL 4  

void stcc4_task(void* args)
{
    i2c_master_bus_handle_t bus_handle;
    STCC4_t stcc4 = {0};  
    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_MASTER_NUM,
        .sda_io_num = I2C_MASTER_SDA,
        .scl_io_num = I2C_MASTER_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, bus_handle));
    
    if (i2c_master_probe(bus_handle, STCC4_I2C_ADDR_64, 1000) == ESP_OK)
    {
        ESP_LOGI(APP_TAG, "STCC4 found at address 0x%02X", STCC4_I2C_ADDR_64);
        ESP_ERROR_CHECK(stcc4_i2c_init(bus_handle));
    }
    else
    {
        ESP_LOGE(APP_TAG, "STCC4 not found!");
        vTaskDelete(NULL);
    }
    while (1)
    {
        stcc4_exit_sleep_mode();
        ESP_ERROR_CHECK(stcc4_measure_single_shot());
        ESP_ERROR_CHECK(stcc4_read_measurement(&stcc4.co2Concentration, &stcc4.temperature, &stcc4.relativeHumidity, &stcc4.sensorStatus));
        ESP_ERROR_CHECK(stcc4_enter_sleep_mode()); 
        ESP_LOGI(TAG, "[single shot] CO2 Concentration: %d ppm, Temperature: %.2f C, Relative Humidity: %.2f %%, Sensor Status: 0x%04X",stcc4.co2Concentration, stcc4.temperature, stcc4.relativeHumidity, stcc4.sensorStatus);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
```