#include "ssd1306.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "ssd1306";

// Simple 5x7 font (ASCII characters 0-9, :, ., -, space, c)
// Each character is 5 pixels wide, 7 pixels high, stored in 5 bytes (one byte per column, referenced from demo project)
static const uint8_t font_5x7[][5] = {
    // '0' - Consistent with demo project
    {0x3E, 0x51, 0x49, 0x45, 0x3E},
    // '1'
    {0x00, 0x42, 0x7F, 0x40, 0x00},
    // '2'
    {0x42, 0x61, 0x51, 0x49, 0x46},
    // '3'
    {0x21, 0x41, 0x45, 0x4B, 0x31},
    // '4'
    {0x18, 0x14, 0x12, 0x7F, 0x10},
    // '5'
    {0x27, 0x45, 0x45, 0x45, 0x39},
    // '6'
    {0x3C, 0x4A, 0x49, 0x49, 0x30},
    // '7'
    {0x01, 0x71, 0x09, 0x05, 0x03},
    // '8'
    {0x36, 0x49, 0x49, 0x49, 0x36},
    // '9'
    {0x06, 0x49, 0x49, 0x29, 0x1E},
    // ':'
    {0x00, 0x36, 0x36, 0x00, 0x00},
    // '.' (decimal point)
    {0x00, 0x00, 0x00, 0x00, 0x60},
    // '-' (minus sign)
    {0x08, 0x08, 0x08, 0x08, 0x08},
    // ' ' (space)
    {0x00, 0x00, 0x00, 0x00, 0x00},
    // 'c' (lowercase c) - Reverse bit order to match current parsing method
    // Demo original data: {0x38, 0x44, 0x44, 0x44, 0x20}
    // Reverse bit order (bits 0-6): 0x38(0011100) -> 0x1C(0001110), 0x44(0100010) -> 0x22(0100010), 0x20(0010000) -> 0x04(0000100)
    {0x1C, 0x22, 0x22, 0x22, 0x04},
};

// Send command to SSD1306
static bool ssd1306_write_cmd(ssd1306_t *ssd1306, uint8_t cmd) {
    if (!ssd1306 || !ssd1306->i2c_dev) {
        return false;
    }
    
    uint8_t data[2] = {SSD1306_CMD_MODE, cmd};
    esp_err_t ret = i2c_master_transmit(ssd1306->i2c_dev, data, 2, pdMS_TO_TICKS(500));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write command 0x%02X: %s", cmd, esp_err_to_name(ret));
    }
    return ret == ESP_OK;
}

// Send data to SSD1306
static bool ssd1306_write_data(ssd1306_t *ssd1306, const uint8_t *data, size_t len) {
    if (!ssd1306 || !ssd1306->i2c_dev || !data) {
        return false;
    }
    
    // Use static buffer to avoid malloc
    // Send at most 128 bytes (one page width) + 1 control byte each time
    static uint8_t packet[129];  // Maximum 128 bytes data + 1 control byte
    
    size_t offset = 0;
    while (offset < len) {
        size_t chunk_size = (len - offset > 128) ? 128 : (len - offset);
        
        // Prepare packet: first byte is data mode flag
        packet[0] = SSD1306_DATA_MODE;
        memcpy(packet + 1, data + offset, chunk_size);
        
        // Increase timeout to 1000ms and add retry mechanism
        esp_err_t ret = ESP_FAIL;
        for (int retry = 0; retry < 3; retry++) {
            ret = i2c_master_transmit(ssd1306->i2c_dev, packet, chunk_size + 1, pdMS_TO_TICKS(1000));
            if (ret == ESP_OK) {
                break;
            }
            if (retry < 2) {
                vTaskDelay(pdMS_TO_TICKS(10));  // Wait before retry
            }
        }
        
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to write data chunk at offset %d after 3 retries: %s", offset, esp_err_to_name(ret));
            return false;
        }
        
        offset += chunk_size;
        
        // Small delay to ensure I2C bus stability
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    
    return true;
}

// Initialize SSD1306
bool ssd1306_init(ssd1306_t *ssd1306, i2c_master_bus_handle_t i2c_bus, uint8_t i2c_addr) {
    if (!ssd1306 || !i2c_bus) {
        return false;
    }
    
    // Create I2C device
    // Note: Use 100kHz to match DS3231 and avoid I2C bus conflicts
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = i2c_addr,
        .scl_speed_hz = 100000,  // 100kHz, consistent with DS3231
    };
    
    esp_err_t ret = i2c_master_bus_add_device(i2c_bus, &dev_cfg, &ssd1306->i2c_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add I2C device: %s", esp_err_to_name(ret));
        return false;
    }
    
    ssd1306->i2c_bus = i2c_bus;
    ssd1306->i2c_addr = i2c_addr;
    
    // Initialize display buffer
    memset(ssd1306->buffer, 0, sizeof(ssd1306->buffer));
    
    // Send initialization command sequence
    vTaskDelay(pdMS_TO_TICKS(100));  // Wait for hardware to stabilize, increase delay
    
    // Turn off display
    if (!ssd1306_write_cmd(ssd1306, SSD1306_CMD_DISPLAY_OFF)) {
        ESP_LOGE(TAG, "Failed to send DISPLAY_OFF command");
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
    
    // Set display clock divider and oscillator frequency
    if (!ssd1306_write_cmd(ssd1306, SSD1306_CMD_SET_DISPLAY_CLOCK)) return false;
    if (!ssd1306_write_cmd(ssd1306, 0x80)) return false;  // Recommended value
    
    // Set multiplexer
    if (!ssd1306_write_cmd(ssd1306, SSD1306_CMD_SET_MULTIPLEX)) return false;
    if (!ssd1306_write_cmd(ssd1306, SSD1306_HEIGHT - 1)) return false;  // 64-1 = 63
    
    // Set display offset
    if (!ssd1306_write_cmd(ssd1306, SSD1306_CMD_SET_DISPLAY_OFFSET)) return false;
    if (!ssd1306_write_cmd(ssd1306, 0x00)) return false;
    
    // Set start line
    if (!ssd1306_write_cmd(ssd1306, SSD1306_CMD_SET_START_LINE | 0x00)) return false;
    
    // Enable charge pump
    if (!ssd1306_write_cmd(ssd1306, SSD1306_CMD_CHARGE_PUMP)) return false;
    if (!ssd1306_write_cmd(ssd1306, 0x14)) return false;  // Enable internal VCC
    
    // Set memory address mode (horizontal address mode, 0x00)
    if (!ssd1306_write_cmd(ssd1306, SSD1306_CMD_MEMORY_MODE)) return false;
    if (!ssd1306_write_cmd(ssd1306, 0x00)) return false;  // Horizontal address mode (works like ks0108)
    
    // Segment remap (horizontal flip)
    if (!ssd1306_write_cmd(ssd1306, SSD1306_CMD_SEG_REMAP | 0x01)) return false;
    
    // COM scan direction (vertical flip)
    if (!ssd1306_write_cmd(ssd1306, SSD1306_CMD_COM_SCAN_DEC)) return false;
    
    // Set COM pin configuration
    if (!ssd1306_write_cmd(ssd1306, SSD1306_CMD_SET_COM_PINS)) return false;
    if (!ssd1306_write_cmd(ssd1306, 0x12)) return false;  // 128x64 configuration
    
    // Set contrast
    if (!ssd1306_write_cmd(ssd1306, SSD1306_CMD_SET_CONTRAST)) return false;
    if (!ssd1306_write_cmd(ssd1306, 0xCF)) return false;  // Contrast value
    
    // Set precharge period
    if (!ssd1306_write_cmd(ssd1306, SSD1306_CMD_SET_PRECHARGE)) return false;
    if (!ssd1306_write_cmd(ssd1306, 0xF1)) return false;  // Recommended value
    
    // Set VCOMH deselect level
    if (!ssd1306_write_cmd(ssd1306, SSD1306_CMD_SET_VCOM_DETECT)) return false;
    if (!ssd1306_write_cmd(ssd1306, 0x40)) return false;  // Recommended value
    
    // Display all pixels resume (important: avoid snow screen)
    if (!ssd1306_write_cmd(ssd1306, SSD1306_CMD_DISPLAY_ALL_ON_RESUME)) return false;
    
    // Normal display (non-inverted)
    if (!ssd1306_write_cmd(ssd1306, SSD1306_CMD_NORMAL_DISPLAY)) return false;
    
    // Deactivate scrolling
    if (!ssd1306_write_cmd(ssd1306, SSD1306_CMD_DEACTIVATE_SCROLL)) return false;
    
    // Clear screen (don't refresh yet, wait until initialization completes)
    ssd1306_clear(ssd1306);
    
    // Turn on display
    if (!ssd1306_write_cmd(ssd1306, SSD1306_CMD_DISPLAY_ON)) {
        ESP_LOGE(TAG, "Failed to enable display");
        return false;
    }
    
    vTaskDelay(pdMS_TO_TICKS(50));  // Wait for display to stabilize
    
    ESP_LOGI(TAG, "SSD1306 initialized successfully (I2C addr: 0x%02X)", i2c_addr);
    ESP_LOGI(TAG, "Note: First refresh will happen when time is displayed");
    return true;
}

// Clear display buffer
void ssd1306_clear(ssd1306_t *ssd1306) {
    if (!ssd1306) {
        return;
    }
    memset(ssd1306->buffer, 0, sizeof(ssd1306->buffer));
}

// Refresh display buffer to screen
bool ssd1306_refresh(ssd1306_t *ssd1306) {
    if (!ssd1306 || !ssd1306->i2c_dev) {
        return false;
    }
    
    // Set page address (0-7)
    if (!ssd1306_write_cmd(ssd1306, SSD1306_CMD_PAGE_ADDR)) return false;
    if (!ssd1306_write_cmd(ssd1306, 0)) return false;      // Start page
    if (!ssd1306_write_cmd(ssd1306, SSD1306_PAGES - 1)) return false;  // End page
    
    // Set column address (0-127)
    if (!ssd1306_write_cmd(ssd1306, SSD1306_CMD_COLUMN_ADDR)) return false;
    if (!ssd1306_write_cmd(ssd1306, 0)) return false;      // Start column
    if (!ssd1306_write_cmd(ssd1306, SSD1306_WIDTH - 1)) return false;  // End column
    
    // Send entire buffer (send at once, referenced from demo project)
    // Use static buffer to avoid stack overflow
    static uint8_t data_packet[1025];  // 1024 bytes data + 1 control byte
    data_packet[0] = SSD1306_DATA_MODE;  // 0x40 data mode
    memcpy(data_packet + 1, ssd1306->buffer, sizeof(ssd1306->buffer));
    
    // Send entire buffer at once
    esp_err_t ret = ESP_FAIL;
    for (int retry = 0; retry < 3; retry++) {
        ret = i2c_master_transmit(ssd1306->i2c_dev, data_packet, sizeof(ssd1306->buffer) + 1, pdMS_TO_TICKS(2000));
        if (ret == ESP_OK) {
            break;
        }
        if (retry < 2) {
            vTaskDelay(pdMS_TO_TICKS(20));  // Wait before retry
        }
    }
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to refresh display after 3 retries: %s", esp_err_to_name(ret));
        return false;
    }
    
    return true;
}

// Draw a character in buffer (supports scaling)
static void ssd1306_draw_char(ssd1306_t *ssd1306, uint8_t x, uint8_t y, char c, uint8_t size) {
    if (!ssd1306 || x >= SSD1306_WIDTH || y >= SSD1306_HEIGHT || size == 0) {
        return;
    }
    
    // Get character index
    uint8_t char_idx = 0xFF;
    if (c >= '0' && c <= '9') {
        char_idx = c - '0';
    } else if (c == ':') {
        char_idx = 10;
    } else if (c == '.') {
        char_idx = 11;
    } else if (c == '-') {
        char_idx = 12;
    } else if (c == ' ') {
        char_idx = 13;
    } else if (c == 'c' || c == 'C') {
        char_idx = 14;  // Support both uppercase and lowercase 'c'
    } else {
        return;  // Unsupported character
    }
    
    // Draw character (5x7 font)
    // Font data format: one byte per column, 5 columns total
    const uint8_t *font_data = font_5x7[char_idx];
    uint8_t char_width = 5;
    uint8_t char_height = 7;
    
    // Draw by column (one byte per column)
    for (uint8_t col = 0; col < char_width; col++) {
        uint8_t font_byte = font_data[col];
        // Draw pixels in this column
        for (uint8_t row = 0; row < char_height; row++) {
            if (font_byte & (1 << row)) {  // Start from least significant bit (row 0 corresponds to bit 0)
                // If size > 1, need to scale drawing
                for (uint8_t sy = 0; sy < size; sy++) {
                    for (uint8_t sx = 0; sx < size; sx++) {
                        uint8_t px = x + col * size + sx;
                        uint8_t py = y + row * size + sy;
                        if (px < SSD1306_WIDTH && py < SSD1306_HEIGHT) {
                            // Calculate position in buffer (page mode)
                            uint8_t page = py / 8;
                            uint8_t bit = py % 8;
                            if (page < SSD1306_PAGES) {
                                ssd1306->buffer[page * SSD1306_WIDTH + px] |= (1 << bit);
                            }
                        }
                    }
                }
            }
        }
    }
}

// Calculate string display width (pixels)
static uint8_t ssd1306_get_string_width(const char *text, uint8_t size) {
    if (!text) {
        return 0;
    }
    
    uint8_t char_width = 5 * size;
    // For large fonts (size >= 4), use compact spacing (1 pixel) to avoid exceeding screen
    uint8_t char_spacing = (size >= 4) ? 1 : (1 * size);
    
    size_t len = strlen(text);
    if (len == 0) {
        return 0;
    }
    
    // Total width = sum of character widths + sum of character spacings
    return len * char_width + (len - 1) * char_spacing;
}

// Display string
bool ssd1306_draw_string(ssd1306_t *ssd1306, uint8_t x, uint8_t y, const char *text, uint8_t size) {
    if (!ssd1306 || !text) {
        return false;
    }
    
    uint8_t char_width = 5 * size;
    // For large fonts (size >= 4), use compact spacing (1 pixel) to avoid exceeding screen
    uint8_t char_spacing = (size >= 4) ? 1 : (1 * size);
    uint8_t current_x = x;
    
    for (const char *p = text; *p != '\0'; p++) {
        if (current_x + char_width > SSD1306_WIDTH) {
            break;  // Exceed screen width
        }
        
        ssd1306_draw_char(ssd1306, current_x, y, *p, size);
        current_x += char_width + char_spacing;
    }
    
    return true;
}

// Display time string (format: hh:mm or hh:mm:ss)
bool ssd1306_show_time(ssd1306_t *ssd1306, const char *time_str) {
    if (!ssd1306 || !time_str) {
        return false;
    }
    
    // Clear buffer
    ssd1306_clear(ssd1306);
    
    // Calculate center position (assuming 2x size font)
    // Time string length: 5 characters (hh:mm) or 8 characters (hh:mm:ss)
    // Character width: 5*2 = 10 pixels, spacing: 1*2 = 2 pixels
    // For hh:mm: total width = 5*10 + 4*2 = 50 + 8 = 58 pixels, center X = (128-58)/2 = 35
    // For hh:mm:ss: total width = 8*10 + 7*2 = 80 + 14 = 94 pixels, center X = (128-94)/2 = 17
    // Use general calculation: dynamically calculate based on string length
    size_t len = strlen(time_str);
    uint8_t total_width = len * 10 + (len - 1) * 2;  // Assume 2x font
    uint8_t x = (SSD1306_WIDTH - total_width) / 2;
    uint8_t y = 28;  // Vertical center (64/2 - 7*2/2 = 32 - 7 = 25, slightly adjusted to 28)
    
    // Draw time string (using 2x size)
    ssd1306_draw_string(ssd1306, x, y, time_str, 2);
    
    // Refresh to screen
    return ssd1306_refresh(ssd1306);
}

// Display complete clock interface (time, date, temperature)
// Layout referenced from demo project: top shows date/weekday/temperature, bottom shows time
bool ssd1306_show_clock(ssd1306_t *ssd1306, const char *time_str, const char *date_str, const char *weekday_str, const char *temp_str, int8_t offset_x, int8_t offset_y) {
    if (!ssd1306 || !time_str) {
        return false;
    }
    
    // Limit offset range to prevent exceeding screen
    if (offset_x < -2) offset_x = -2;
    if (offset_x > 2) offset_x = 2;
    if (offset_y < -2) offset_y = -2;
    if (offset_y > 2) offset_y = 2;
    
    // Clear buffer
    ssd1306_clear(ssd1306);
    
    // Top area: display date, weekday and temperature
    // Row 1: Date (left-aligned, size=2) + Temperature (right-aligned, size=1, top-right corner)
    int16_t top_y = 1 + offset_y;  // Top Y coordinate, apply Y offset
    if (top_y < 0) top_y = 0;
    if (top_y >= SSD1306_HEIGHT) top_y = SSD1306_HEIGHT - 1;
    uint8_t date_font_size = 2;  // Date font size: 2x
    uint8_t temp_font_size = 1;   // Temperature font size: 1x (original size)
    
    if (date_str) {
        // Date left-aligned, apply X offset
        int16_t date_x = 2 + offset_x;
        if (date_x < 0) date_x = 0;
        if (date_x >= SSD1306_WIDTH) date_x = SSD1306_WIDTH - 1;
        ssd1306_draw_string(ssd1306, (uint8_t)date_x, (uint8_t)top_y, date_str, date_font_size);
    }
    
    if (temp_str) {
        // Calculate temperature string width (for right alignment, using size=1)
        uint8_t temp_width = ssd1306_get_string_width(temp_str, temp_font_size);
        // Right-aligned, top-right corner of screen, leave 1 pixel margin, ensure not exceeding screen, apply X offset
        int16_t temp_x = SSD1306_WIDTH - temp_width - 1 + offset_x;
        if (temp_x < 0) temp_x = 0;
        if (temp_x >= SSD1306_WIDTH) temp_x = SSD1306_WIDTH - 1;
        // Temperature Y coordinate moved down 20 pixels, apply Y offset
        int16_t temp_y = top_y + 20 + offset_y;
        if (temp_y < 0) temp_y = 0;
        if (temp_y >= SSD1306_HEIGHT) temp_y = SSD1306_HEIGHT - 1;
        ssd1306_draw_string(ssd1306, (uint8_t)temp_x, (uint8_t)temp_y, temp_str, temp_font_size);
    }
    
    // Row 2: Weekday (left-aligned), using 2x font
    if (weekday_str) {
        int16_t weekday_y = top_y + 7 * date_font_size + 2 + offset_y;  // Date row height + spacing, apply Y offset
        if (weekday_y < 0) weekday_y = 0;
        if (weekday_y >= SSD1306_HEIGHT) weekday_y = SSD1306_HEIGHT - 1;
        int16_t weekday_x = 2 + offset_x;  // Apply X offset
        if (weekday_x < 0) weekday_x = 0;
        if (weekday_x >= SSD1306_WIDTH) weekday_x = SSD1306_WIDTH - 1;
        ssd1306_draw_string(ssd1306, (uint8_t)weekday_x, (uint8_t)weekday_y, weekday_str, date_font_size);
    }
    
    // Bottom area: display time (centered, large font)
    // Use 4x size font, compact spacing (spacing is 1 pixel, not 4 pixels)
    // Time string length: 5 characters (hh:mm, seconds not displayed)
    // Character width: 5*4 = 20 pixels, spacing: 1 pixel (compact layout)
    // Total width: 5*20 + 4*1 = 100 + 4 = 104 pixels
    // Center X coordinate: (128 - 104) / 2 = 12
    // Calculate time display Y coordinate: top area occupies about 32 pixels (date row 14 + spacing 2 + weekday row 14 + spacing 2)
    // Time font height: 7*4 = 28 pixels, vertically centered in remaining 32 pixels: 32 + (32-28)/2 = 34
    int16_t x = 12 + offset_x;  // Center display, apply X offset
    if (x < 0) x = 0;
    if (x >= SSD1306_WIDTH) x = SSD1306_WIDTH - 1;
    int16_t y = 34 + offset_y;  // Adjust Y coordinate to avoid overlapping with top area, apply Y offset
    if (y < 0) y = 0;
    if (y >= SSD1306_HEIGHT) y = SSD1306_HEIGHT - 1;
    
    // Draw time string (using 4x size, draw_string will automatically use compact spacing)
    ssd1306_draw_string(ssd1306, (uint8_t)x, (uint8_t)y, time_str, 4);
    
    // Refresh to screen
    return ssd1306_refresh(ssd1306);
}

// Set display on/off
bool ssd1306_set_display_on(ssd1306_t *ssd1306, bool on) {
    if (!ssd1306) {
        return false;
    }
    return ssd1306_write_cmd(ssd1306, on ? SSD1306_CMD_DISPLAY_ON : SSD1306_CMD_DISPLAY_OFF);
}

// Set contrast
bool ssd1306_set_contrast(ssd1306_t *ssd1306, uint8_t contrast) {
    if (!ssd1306) {
        return false;
    }
    if (!ssd1306_write_cmd(ssd1306, SSD1306_CMD_SET_CONTRAST)) {
        return false;
    }
    return ssd1306_write_cmd(ssd1306, contrast);
}
