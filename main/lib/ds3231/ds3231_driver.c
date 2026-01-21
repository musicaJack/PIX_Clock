#include "ds3231.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "ds3231";

// Write register
static bool ds3231_write_register(ds3231_t *ds3231, uint8_t reg, uint8_t value) {
    if (!ds3231 || !ds3231->i2c_dev) {
        return false;
    }
    
    uint8_t data[2] = {reg, value};
    esp_err_t ret = i2c_master_transmit(ds3231->i2c_dev, data, 2, pdMS_TO_TICKS(100));
    return ret == ESP_OK;
}

// Read register
static bool ds3231_read_register(ds3231_t *ds3231, uint8_t reg, uint8_t *value) {
    if (!ds3231 || !ds3231->i2c_dev || !value) {
        return false;
    }
    
    // Write register address
    esp_err_t ret = i2c_master_transmit(ds3231->i2c_dev, &reg, 1, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        return false;
    }
    
    // Read data
    ret = i2c_master_receive(ds3231->i2c_dev, value, 1, pdMS_TO_TICKS(100));
    return ret == ESP_OK;
}

// Initialize DS3231
bool ds3231_init(ds3231_t *ds3231, i2c_master_bus_handle_t i2c_bus, uint8_t sda_pin, uint8_t scl_pin) {
    if (!ds3231 || !i2c_bus) {
        return false;
    }
    
    // Create I2C device
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = DS3231_I2C_ADDR,
        .scl_speed_hz = 100000,  // 100kHz
    };
    
    esp_err_t ret = i2c_master_bus_add_device(i2c_bus, &dev_cfg, &ds3231->i2c_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add I2C device: %s", esp_err_to_name(ret));
        return false;
    }
    
    ds3231->i2c_bus = i2c_bus;
    
    // Enable oscillator
    return ds3231_enable_oscillator(ds3231, true);
}

// Read time
bool ds3231_read_time(ds3231_t *ds3231, ds3231_time_t *time) {
    if (!ds3231 || !ds3231->i2c_dev || !time) {
        return false;
    }
    
    uint8_t reg = DS3231_SECONDS_REG;
    uint8_t data[7];
    
    // Write starting register address
    esp_err_t ret = i2c_master_transmit(ds3231->i2c_dev, &reg, 1, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write register address: %s", esp_err_to_name(ret));
        return false;
    }
    
    // Read 7 bytes of time data
    ret = i2c_master_receive(ds3231->i2c_dev, data, 7, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read time: %s", esp_err_to_name(ret));
        return false;
    }
    
    // Parse time data
    time->seconds = bcd_to_bin(data[0] & 0x7F);
    time->minutes = bcd_to_bin(data[1] & 0x7F);
    time->hours = bcd_to_bin(data[2] & 0x3F);
    time->day = data[3] & 0x07;  // Day register directly stores 1-7, no BCD conversion needed
    time->date = bcd_to_bin(data[4] & 0x3F);
    time->month = bcd_to_bin(data[5] & 0x1F);
    time->year = bcd_to_bin(data[6]);
    
    return true;
}

// Write time
bool ds3231_write_time(ds3231_t *ds3231, const ds3231_time_t *time) {
    if (!ds3231 || !ds3231->i2c_dev || !time) {
        return false;
    }
    
    uint8_t data[8];
    data[0] = DS3231_SECONDS_REG;
    data[1] = bin_to_bcd(time->seconds);
    data[2] = bin_to_bcd(time->minutes);
    data[3] = bin_to_bcd(time->hours);
    data[4] = time->day & 0x07;  // Day register directly stores 1-7, ensure range is correct
    data[5] = bin_to_bcd(time->date);
    data[6] = bin_to_bcd(time->month);
    data[7] = bin_to_bcd(time->year);
    
    esp_err_t ret = i2c_master_transmit(ds3231->i2c_dev, data, 8, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write time: %s", esp_err_to_name(ret));
        return false;
    }
    return true;
}

// Read temperature
bool ds3231_read_temperature(ds3231_t *ds3231, float *temperature) {
    if (!ds3231 || !ds3231->i2c_dev || !temperature) {
        return false;
    }
    
    uint8_t msb, lsb;
    
    if (!ds3231_read_register(ds3231, DS3231_TEMP_MSB, &msb)) {
        return false;
    }
    
    if (!ds3231_read_register(ds3231, DS3231_TEMP_LSB, &lsb)) {
        return false;
    }
    
    // Temperature calculation: MSB is integer part, upper 2 bits of LSB are fractional part
    int16_t temp_raw = (int16_t)((msb << 8) | lsb);
    temp_raw >>= 6; // Right shift 6 bits, because lower 6 bits of LSB are unused
    
    *temperature = temp_raw * 0.25f;
    return true;
}

// Enable/disable oscillator
bool ds3231_enable_oscillator(ds3231_t *ds3231, bool enable) {
    if (!ds3231 || !ds3231->i2c_dev) {
        return false;
    }
    
    uint8_t control_reg;
    if (!ds3231_read_register(ds3231, DS3231_CONTROL_REG, &control_reg)) {
        return false;
    }
    
    if (enable) {
        control_reg &= ~(1 << DS3231_EOSC_BIT); // Clear EOSC bit to enable oscillator
    } else {
        control_reg |= (1 << DS3231_EOSC_BIT);  // Set EOSC bit to disable oscillator
    }
    
    return ds3231_write_register(ds3231, DS3231_CONTROL_REG, control_reg);
}

// Check if oscillator is stopped
bool ds3231_is_oscillator_stopped(ds3231_t *ds3231, bool *stopped) {
    if (!ds3231 || !ds3231->i2c_dev || !stopped) {
        return false;
    }
    
    uint8_t status_reg;
    if (!ds3231_read_register(ds3231, DS3231_STATUS_REG, &status_reg)) {
        return false;
    }
    
    *stopped = (status_reg & (1 << DS3231_OSF_BIT)) != 0;
    return true;
}

// Convert time to string
void ds3231_time_to_string(const ds3231_time_t *time, char *buffer, size_t buffer_size) {
    if (!time || !buffer || buffer_size < 20) {
        return;
    }
    
    const char* day_names[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    
    snprintf(buffer, buffer_size, "%s %04d-%02d-%02d %02d:%02d:%02d",
             day_names[time->day - 1],
             2000 + time->year,
             time->month,
             time->date,
             time->hours,
             time->minutes,
             time->seconds);
}

// BCD to binary
uint8_t bcd_to_bin(uint8_t bcd) {
    return ((bcd >> 4) * 10) + (bcd & 0x0F);
}

// Binary to BCD
uint8_t bin_to_bcd(uint8_t bin) {
    return ((bin / 10) << 4) | (bin % 10);
}
