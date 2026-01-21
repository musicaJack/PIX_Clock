#ifndef SSD1306_H
#define SSD1306_H

#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include <stdint.h>
#include <stdbool.h>

// SSD1306 I2C地址（常见为0x3C或0x3D）
#define SSD1306_I2C_ADDR_0    0x3C
#define SSD1306_I2C_ADDR_1    0x3D

// SSD1306 显示尺寸
#define SSD1306_WIDTH         128
#define SSD1306_HEIGHT        64
#define SSD1306_PAGES         8  // 64像素高度 / 8像素每页 = 8页

// SSD1306命令定义
#define SSD1306_CMD_MODE       0x00
#define SSD1306_DATA_MODE      0x40

// SSD1306命令
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

// SSD1306设备结构体
typedef struct {
    i2c_master_bus_handle_t i2c_bus;
    i2c_master_dev_handle_t i2c_dev;
    uint8_t i2c_addr;
    uint8_t buffer[SSD1306_WIDTH * SSD1306_PAGES];  // 显示缓冲区 (128 * 8 = 1024字节)
} ssd1306_t;

/**
 * @brief 初始化SSD1306显示模块
 * 
 * @param ssd1306 SSD1306设备结构体指针
 * @param i2c_bus I2C总线句柄（与DS3231共用）
 * @param i2c_addr I2C地址（0x3C或0x3D）
 * @return true 成功，false 失败
 */
bool ssd1306_init(ssd1306_t *ssd1306, i2c_master_bus_handle_t i2c_bus, uint8_t i2c_addr);

/**
 * @brief 清空显示缓冲区
 * 
 * @param ssd1306 SSD1306设备结构体指针
 */
void ssd1306_clear(ssd1306_t *ssd1306);

/**
 * @brief 刷新显示缓冲区到屏幕
 * 
 * @param ssd1306 SSD1306设备结构体指针
 * @return true 成功，false 失败
 */
bool ssd1306_refresh(ssd1306_t *ssd1306);

/**
 * @brief 显示字符串（使用内置字体）
 * 
 * @param ssd1306 SSD1306设备结构体指针
 * @param x 起始X坐标（0-127）
 * @param y 起始Y坐标（0-63，以像素为单位）
 * @param text 要显示的字符串
 * @param size 字体大小（1或2）
 * @return true 成功，false 失败
 */
bool ssd1306_draw_string(ssd1306_t *ssd1306, uint8_t x, uint8_t y, const char *text, uint8_t size);

/**
 * @brief 显示时间字符串（格式：hh:mm:ss）
 * 
 * @param ssd1306 SSD1306设备结构体指针
 * @param time_str 时间字符串（格式：hh:mm:ss）
 * @return true 成功，false 失败
 */
bool ssd1306_show_time(ssd1306_t *ssd1306, const char *time_str);

/**
 * @brief 显示完整时钟界面（时间、日期、温度）
 * 
 * @param ssd1306 SSD1306设备结构体指针
 * @param time_str 时间字符串（格式：hh:mm:ss）
 * @param date_str 日期字符串（格式：YYYY-MM-DD）
 * @param weekday_str 星期字符串（格式：Mon/Tue等）
 * @param temp_str 温度字符串（格式：XX.X°C 或 XX.XC）
 * @param offset_x X轴像素偏移量（用于防烧屏，范围：-2到+2）
 * @param offset_y Y轴像素偏移量（用于防烧屏，范围：-2到+2）
 * @return true 成功，false 失败
 */
bool ssd1306_show_clock(ssd1306_t *ssd1306, const char *time_str, const char *date_str, const char *weekday_str, const char *temp_str, int8_t offset_x, int8_t offset_y);

/**
 * @brief 设置显示开关
 * 
 * @param ssd1306 SSD1306设备结构体指针
 * @param on true 开启显示，false 关闭显示
 * @return true 成功，false 失败
 */
bool ssd1306_set_display_on(ssd1306_t *ssd1306, bool on);

/**
 * @brief 设置对比度
 * 
 * @param ssd1306 SSD1306设备结构体指针
 * @param contrast 对比度值（0-255）
 * @return true 成功，false 失败
 */
bool ssd1306_set_contrast(ssd1306_t *ssd1306, uint8_t contrast);

#endif // SSD1306_H
