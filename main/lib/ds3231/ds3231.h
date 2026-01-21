#ifndef DS3231_H
#define DS3231_H

#include "driver/i2c_master.h"
#include <stdint.h>
#include <stdbool.h>

// DS3231 I2C address (fixed at 0x68)
#define DS3231_I2C_ADDR 0x68

// DS3231 register addresses
#define DS3231_SECONDS_REG    0x00
#define DS3231_MINUTES_REG    0x01
#define DS3231_HOURS_REG      0x02
#define DS3231_DAY_REG        0x03
#define DS3231_DATE_REG       0x04
#define DS3231_MONTH_REG      0x05
#define DS3231_YEAR_REG       0x06
#define DS3231_ALARM1_SEC     0x07
#define DS3231_ALARM1_MIN     0x08
#define DS3231_ALARM1_HOUR    0x09
#define DS3231_ALARM1_DAY     0x0A
#define DS3231_ALARM2_MIN     0x0B
#define DS3231_ALARM2_HOUR    0x0C
#define DS3231_ALARM2_DAY     0x0D
#define DS3231_CONTROL_REG    0x0E
#define DS3231_STATUS_REG     0x0F
#define DS3231_AGING_REG      0x10
#define DS3231_TEMP_MSB       0x11
#define DS3231_TEMP_LSB       0x12

// Control register bit definitions
#define DS3231_EOSC_BIT       7  // Enable Oscillator bit

// Status register bit definitions
#define DS3231_OSF_BIT        7  // Oscillator Stop Flag bit

// DS3231 time structure
typedef struct {
    uint8_t seconds;  // 0-59
    uint8_t minutes;  // 0-59
    uint8_t hours;    // 0-23 (24-hour format)
    uint8_t day;      // 1-7 (1=Sunday, 7=Saturday)
    uint8_t date;     // 1-31
    uint8_t month;    // 1-12
    uint8_t year;     // 0-99 (represents 2000-2099)
} ds3231_time_t;

// DS3231 device structure
typedef struct {
    i2c_master_bus_handle_t i2c_bus;
    i2c_master_dev_handle_t i2c_dev;
} ds3231_t;

// Function declarations
bool ds3231_init(ds3231_t *ds3231, i2c_master_bus_handle_t i2c_bus, uint8_t sda_pin, uint8_t scl_pin);
bool ds3231_read_time(ds3231_t *ds3231, ds3231_time_t *time);
bool ds3231_write_time(ds3231_t *ds3231, const ds3231_time_t *time);
bool ds3231_read_temperature(ds3231_t *ds3231, float *temperature);
bool ds3231_enable_oscillator(ds3231_t *ds3231, bool enable);
bool ds3231_is_oscillator_stopped(ds3231_t *ds3231, bool *stopped);
void ds3231_time_to_string(const ds3231_time_t *time, char *buffer, size_t buffer_size);

// Helper functions
uint8_t bcd_to_bin(uint8_t bcd);
uint8_t bin_to_bcd(uint8_t bin);

#endif // DS3231_H
