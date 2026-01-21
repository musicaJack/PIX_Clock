#ifndef SSD1306_H
#define SSD1306_H

#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include <stdint.h>
#include <stdbool.h>

// SSD1306 I2C address (common is 0x3C or 0x3D)
#define SSD1306_I2C_ADDR_0    0x3C
#define SSD1306_I2C_ADDR_1    0x3D

// SSD1306 display dimensions
#define SSD1306_WIDTH         128
#define SSD1306_HEIGHT        64
#define SSD1306_PAGES         8  // 64 pixels height / 8 pixels per page = 8 pages

// SSD1306 command definitions
#define SSD1306_CMD_MODE       0x00
#define SSD1306_DATA_MODE      0x40

// SSD1306 commands
#define SSD1306_CMD_DISPLAY_OFF           0xAE
#define SSD1306_CMD_DISPLAY_ON            0xAF
#define SSD1306_CMD_SET_DISPLAY_CLOCK     0xD5
#define SSD1306_CMD_SET_MULTIPLEX        0xA8
#define SSD1306_CMD_SET_DISPLAY_OFFSET    0xD3
#define SSD1306_CMD_SET_START_LINE       0x40
#define SSD1306_CMD_CHARGE_PUMP           0x8D
#define SSD1306_CMD_MEMORY_MODE           0x20
#define SSD1306_CMD_SEG_REMAP             0xA1
#define SSD1306_CMD_COM_SCAN_INC          0xC0
#define SSD1306_CMD_COM_SCAN_DEC          0xC8
#define SSD1306_CMD_SET_COM_PINS          0xDA
#define SSD1306_CMD_SET_CONTRAST          0x81
#define SSD1306_CMD_SET_PRECHARGE         0xD9
#define SSD1306_CMD_SET_VCOM_DETECT       0xDB
#define SSD1306_CMD_DISPLAY_ALL_ON_RESUME 0xA4
#define SSD1306_CMD_NORMAL_DISPLAY        0xA6
#define SSD1306_CMD_INVERSE_DISPLAY       0xA7
#define SSD1306_CMD_DEACTIVATE_SCROLL     0x2E
#define SSD1306_CMD_ACTIVATE_SCROLL       0x2F
#define SSD1306_CMD_COLUMN_ADDR           0x21
#define SSD1306_CMD_PAGE_ADDR             0x22

// SSD1306 device structure
typedef struct {
    i2c_master_bus_handle_t i2c_bus;
    i2c_master_dev_handle_t i2c_dev;
    uint8_t i2c_addr;
    uint8_t buffer[SSD1306_WIDTH * SSD1306_PAGES];  // Display buffer (128 * 8 = 1024 bytes)
} ssd1306_t;

/**
 * @brief Initialize SSD1306 display module
 * 
 * @param ssd1306 SSD1306 device structure pointer
 * @param i2c_bus I2C bus handle (shared with DS3231)
 * @param i2c_addr I2C address (0x3C or 0x3D)
 * @return true on success, false on failure
 */
bool ssd1306_init(ssd1306_t *ssd1306, i2c_master_bus_handle_t i2c_bus, uint8_t i2c_addr);

/**
 * @brief Clear display buffer
 * 
 * @param ssd1306 SSD1306 device structure pointer
 */
void ssd1306_clear(ssd1306_t *ssd1306);

/**
 * @brief Refresh display buffer to screen
 * 
 * @param ssd1306 SSD1306 device structure pointer
 * @return true on success, false on failure
 */
bool ssd1306_refresh(ssd1306_t *ssd1306);

/**
 * @brief Display string (using built-in font)
 * 
 * @param ssd1306 SSD1306 device structure pointer
 * @param x Start X coordinate (0-127)
 * @param y Start Y coordinate (0-63, in pixels)
 * @param text String to display
 * @param size Font size (1 or 2)
 * @return true on success, false on failure
 */
bool ssd1306_draw_string(ssd1306_t *ssd1306, uint8_t x, uint8_t y, const char *text, uint8_t size);

/**
 * @brief Display time string (format: hh:mm:ss)
 * 
 * @param ssd1306 SSD1306 device structure pointer
 * @param time_str Time string (format: hh:mm:ss)
 * @return true on success, false on failure
 */
bool ssd1306_show_time(ssd1306_t *ssd1306, const char *time_str);

/**
 * @brief Display complete clock interface (time, date, temperature)
 * 
 * @param ssd1306 SSD1306 device structure pointer
 * @param time_str Time string (format: hh:mm:ss)
 * @param date_str Date string (format: YYYY-MM-DD)
 * @param weekday_str Weekday string (format: Mon/Tue etc.)
 * @param temp_str Temperature string (format: XX.X°C or XX.XC)
 * @param offset_x X-axis pixel offset (for burn-in prevention, range: -2 to +2)
 * @param offset_y Y-axis pixel offset (for burn-in prevention, range: -2 to +2)
 * @return true on success, false on failure
 */
bool ssd1306_show_clock(ssd1306_t *ssd1306, const char *time_str, const char *date_str, const char *weekday_str, const char *temp_str, int8_t offset_x, int8_t offset_y);

/**
 * @brief Set display on/off
 * 
 * @param ssd1306 SSD1306 device structure pointer
 * @param on true to turn on display, false to turn off
 * @return true on success, false on failure
 */
bool ssd1306_set_display_on(ssd1306_t *ssd1306, bool on);

/**
 * @brief Set contrast
 * 
 * @param ssd1306 SSD1306 device structure pointer
 * @param contrast Contrast value (0-255)
 * @return true on success, false on failure
 */
bool ssd1306_set_contrast(ssd1306_t *ssd1306, uint8_t contrast);

#endif // SSD1306_H
