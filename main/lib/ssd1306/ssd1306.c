#include "ssd1306.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "ssd1306";

// 简单的5x7字体（ASCII字符0-9、:、.、-、空格、c）
// 每个字符5像素宽，7像素高，存储在5字节中（每字节一列，从demo项目参考）
static const uint8_t font_5x7[][5] = {
    // '0' - 与demo项目一致
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
    // '.' (小数点)
    {0x00, 0x00, 0x00, 0x00, 0x60},
    // '-' (减号)
    {0x08, 0x08, 0x08, 0x08, 0x08},
    // ' ' (空格)
    {0x00, 0x00, 0x00, 0x00, 0x00},
    // 'c' (小写字母c) - 反转bit顺序以匹配当前解析方式
    // demo原始数据: {0x38, 0x44, 0x44, 0x44, 0x20}
    // 反转bit顺序（bit 0-6）：0x38(0011100) -> 0x1C(0001110), 0x44(0100010) -> 0x22(0100010), 0x20(0010000) -> 0x04(0000100)
    {0x1C, 0x22, 0x22, 0x22, 0x04},
};

// 发送命令到SSD1306
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

// 发送数据到SSD1306
static bool ssd1306_write_data(ssd1306_t *ssd1306, const uint8_t *data, size_t len) {
    if (!ssd1306 || !ssd1306->i2c_dev || !data) {
        return false;
    }
    
    // 使用静态缓冲区，避免malloc
    // 每次最多发送128字节（一页的宽度）+ 1字节控制字节
    static uint8_t packet[129];  // 最大128字节数据 + 1字节控制
    
    size_t offset = 0;
    while (offset < len) {
        size_t chunk_size = (len - offset > 128) ? 128 : (len - offset);
        
        // 准备数据包：第一个字节是数据模式标志
        packet[0] = SSD1306_DATA_MODE;
        memcpy(packet + 1, data + offset, chunk_size);
        
        // 增加超时时间到1000ms，并添加重试机制
        esp_err_t ret = ESP_FAIL;
        for (int retry = 0; retry < 3; retry++) {
            ret = i2c_master_transmit(ssd1306->i2c_dev, packet, chunk_size + 1, pdMS_TO_TICKS(1000));
            if (ret == ESP_OK) {
                break;
            }
            if (retry < 2) {
                vTaskDelay(pdMS_TO_TICKS(10));  // 重试前等待
            }
        }
        
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to write data chunk at offset %d after 3 retries: %s", offset, esp_err_to_name(ret));
            return false;
        }
        
        offset += chunk_size;
        
        // 小延迟，确保I2C总线稳定
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    
    return true;
}

// 初始化SSD1306
bool ssd1306_init(ssd1306_t *ssd1306, i2c_master_bus_handle_t i2c_bus, uint8_t i2c_addr) {
    if (!ssd1306 || !i2c_bus) {
        return false;
    }
    
    // 创建I2C设备
    // 注意：使用100kHz与DS3231保持一致，避免I2C总线冲突
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = i2c_addr,
        .scl_speed_hz = 100000,  // 100kHz，与DS3231保持一致
    };
    
    esp_err_t ret = i2c_master_bus_add_device(i2c_bus, &dev_cfg, &ssd1306->i2c_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add I2C device: %s", esp_err_to_name(ret));
        return false;
    }
    
    ssd1306->i2c_bus = i2c_bus;
    ssd1306->i2c_addr = i2c_addr;
    
    // 初始化显示缓冲区
    memset(ssd1306->buffer, 0, sizeof(ssd1306->buffer));
    
    // 发送初始化命令序列
    vTaskDelay(pdMS_TO_TICKS(100));  // 等待硬件稳定，增加延迟
    
    // 关闭显示
    if (!ssd1306_write_cmd(ssd1306, SSD1306_CMD_DISPLAY_OFF)) {
        ESP_LOGE(TAG, "Failed to send DISPLAY_OFF command");
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
    
    // 设置显示时钟分频和振荡器频率
    if (!ssd1306_write_cmd(ssd1306, SSD1306_CMD_SET_DISPLAY_CLOCK)) return false;
    if (!ssd1306_write_cmd(ssd1306, 0x80)) return false;  // 建议值
    
    // 设置多路复用器
    if (!ssd1306_write_cmd(ssd1306, SSD1306_CMD_SET_MULTIPLEX)) return false;
    if (!ssd1306_write_cmd(ssd1306, SSD1306_HEIGHT - 1)) return false;  // 64-1 = 63
    
    // 设置显示偏移
    if (!ssd1306_write_cmd(ssd1306, SSD1306_CMD_SET_DISPLAY_OFFSET)) return false;
    if (!ssd1306_write_cmd(ssd1306, 0x00)) return false;
    
    // 设置起始行
    if (!ssd1306_write_cmd(ssd1306, SSD1306_CMD_SET_START_LINE | 0x00)) return false;
    
    // 启用电荷泵
    if (!ssd1306_write_cmd(ssd1306, SSD1306_CMD_CHARGE_PUMP)) return false;
    if (!ssd1306_write_cmd(ssd1306, 0x14)) return false;  // 启用内部VCC
    
    // 设置内存地址模式（水平地址模式，0x00）
    if (!ssd1306_write_cmd(ssd1306, SSD1306_CMD_MEMORY_MODE)) return false;
    if (!ssd1306_write_cmd(ssd1306, 0x00)) return false;  // 水平地址模式（像ks0108一样工作）
    
    // 段重映射（水平翻转）
    if (!ssd1306_write_cmd(ssd1306, SSD1306_CMD_SEG_REMAP | 0x01)) return false;
    
    // COM扫描方向（垂直翻转）
    if (!ssd1306_write_cmd(ssd1306, SSD1306_CMD_COM_SCAN_DEC)) return false;
    
    // 设置COM引脚配置
    if (!ssd1306_write_cmd(ssd1306, SSD1306_CMD_SET_COM_PINS)) return false;
    if (!ssd1306_write_cmd(ssd1306, 0x12)) return false;  // 128x64配置
    
    // 设置对比度
    if (!ssd1306_write_cmd(ssd1306, SSD1306_CMD_SET_CONTRAST)) return false;
    if (!ssd1306_write_cmd(ssd1306, 0xCF)) return false;  // 对比度值
    
    // 设置预充电周期
    if (!ssd1306_write_cmd(ssd1306, SSD1306_CMD_SET_PRECHARGE)) return false;
    if (!ssd1306_write_cmd(ssd1306, 0xF1)) return false;  // 建议值
    
    // 设置VCOMH取消选择级别
    if (!ssd1306_write_cmd(ssd1306, SSD1306_CMD_SET_VCOM_DETECT)) return false;
    if (!ssd1306_write_cmd(ssd1306, 0x40)) return false;  // 建议值
    
    // 显示所有像素恢复（重要：避免雪花屏）
    if (!ssd1306_write_cmd(ssd1306, SSD1306_CMD_DISPLAY_ALL_ON_RESUME)) return false;
    
    // 正常显示（非反色）
    if (!ssd1306_write_cmd(ssd1306, SSD1306_CMD_NORMAL_DISPLAY)) return false;
    
    // 关闭滚动
    if (!ssd1306_write_cmd(ssd1306, SSD1306_CMD_DEACTIVATE_SCROLL)) return false;
    
    // 清屏（先不刷新，等初始化完成后再刷新）
    ssd1306_clear(ssd1306);
    
    // 开启显示
    if (!ssd1306_write_cmd(ssd1306, SSD1306_CMD_DISPLAY_ON)) {
        ESP_LOGE(TAG, "Failed to enable display");
        return false;
    }
    
    vTaskDelay(pdMS_TO_TICKS(50));  // 等待显示稳定
    
    ESP_LOGI(TAG, "SSD1306 initialized successfully (I2C addr: 0x%02X)", i2c_addr);
    ESP_LOGI(TAG, "Note: First refresh will happen when time is displayed");
    return true;
}

// 清空显示缓冲区
void ssd1306_clear(ssd1306_t *ssd1306) {
    if (!ssd1306) {
        return;
    }
    memset(ssd1306->buffer, 0, sizeof(ssd1306->buffer));
}

// 刷新显示缓冲区到屏幕
bool ssd1306_refresh(ssd1306_t *ssd1306) {
    if (!ssd1306 || !ssd1306->i2c_dev) {
        return false;
    }
    
    // 设置页地址（0-7）
    if (!ssd1306_write_cmd(ssd1306, SSD1306_CMD_PAGE_ADDR)) return false;
    if (!ssd1306_write_cmd(ssd1306, 0)) return false;      // 起始页
    if (!ssd1306_write_cmd(ssd1306, SSD1306_PAGES - 1)) return false;  // 结束页
    
    // 设置列地址（0-127）
    if (!ssd1306_write_cmd(ssd1306, SSD1306_CMD_COLUMN_ADDR)) return false;
    if (!ssd1306_write_cmd(ssd1306, 0)) return false;      // 起始列
    if (!ssd1306_write_cmd(ssd1306, SSD1306_WIDTH - 1)) return false;  // 结束列
    
    // 发送整个缓冲区（一次性发送，参考demo项目）
    // 使用静态缓冲区避免栈溢出
    static uint8_t data_packet[1025];  // 1024字节数据 + 1字节控制
    data_packet[0] = SSD1306_DATA_MODE;  // 0x40 数据模式
    memcpy(data_packet + 1, ssd1306->buffer, sizeof(ssd1306->buffer));
    
    // 一次性发送整个缓冲区
    esp_err_t ret = ESP_FAIL;
    for (int retry = 0; retry < 3; retry++) {
        ret = i2c_master_transmit(ssd1306->i2c_dev, data_packet, sizeof(ssd1306->buffer) + 1, pdMS_TO_TICKS(2000));
        if (ret == ESP_OK) {
            break;
        }
        if (retry < 2) {
            vTaskDelay(pdMS_TO_TICKS(20));  // 重试前等待
        }
    }
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to refresh display after 3 retries: %s", esp_err_to_name(ret));
        return false;
    }
    
    return true;
}

// 在缓冲区中绘制一个字符（支持放大）
static void ssd1306_draw_char(ssd1306_t *ssd1306, uint8_t x, uint8_t y, char c, uint8_t size) {
    if (!ssd1306 || x >= SSD1306_WIDTH || y >= SSD1306_HEIGHT || size == 0) {
        return;
    }
    
    // 获取字符索引
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
        char_idx = 14;  // 支持大小写'c'
    } else {
        return;  // 不支持的字符
    }
    
    // 绘制字符（5x7字体）
    // 字体数据格式：每列一个字节，共5列
    const uint8_t *font_data = font_5x7[char_idx];
    uint8_t char_width = 5;
    uint8_t char_height = 7;
    
    // 按列绘制（每列一个字节）
    for (uint8_t col = 0; col < char_width; col++) {
        uint8_t font_byte = font_data[col];
        // 绘制这一列的像素
        for (uint8_t row = 0; row < char_height; row++) {
            if (font_byte & (1 << row)) {  // 从最低位开始（第0行对应bit 0）
                // 如果size > 1，需要放大绘制
                for (uint8_t sy = 0; sy < size; sy++) {
                    for (uint8_t sx = 0; sx < size; sx++) {
                        uint8_t px = x + col * size + sx;
                        uint8_t py = y + row * size + sy;
                        if (px < SSD1306_WIDTH && py < SSD1306_HEIGHT) {
                            // 计算在缓冲区中的位置（页模式）
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

// 计算字符串的显示宽度（像素）
static uint8_t ssd1306_get_string_width(const char *text, uint8_t size) {
    if (!text) {
        return 0;
    }
    
    uint8_t char_width = 5 * size;
    // 对于大字体（size >= 4），使用紧凑间距（1像素）以避免超出屏幕
    uint8_t char_spacing = (size >= 4) ? 1 : (1 * size);
    
    size_t len = strlen(text);
    if (len == 0) {
        return 0;
    }
    
    // 总宽度 = 字符宽度总和 + 字符间距总和
    return len * char_width + (len - 1) * char_spacing;
}

// 显示字符串
bool ssd1306_draw_string(ssd1306_t *ssd1306, uint8_t x, uint8_t y, const char *text, uint8_t size) {
    if (!ssd1306 || !text) {
        return false;
    }
    
    uint8_t char_width = 5 * size;
    // 对于大字体（size >= 4），使用紧凑间距（1像素）以避免超出屏幕
    uint8_t char_spacing = (size >= 4) ? 1 : (1 * size);
    uint8_t current_x = x;
    
    for (const char *p = text; *p != '\0'; p++) {
        if (current_x + char_width > SSD1306_WIDTH) {
            break;  // 超出屏幕宽度
        }
        
        ssd1306_draw_char(ssd1306, current_x, y, *p, size);
        current_x += char_width + char_spacing;
    }
    
    return true;
}

// 显示时间字符串（格式：hh:mm 或 hh:mm:ss）
bool ssd1306_show_time(ssd1306_t *ssd1306, const char *time_str) {
    if (!ssd1306 || !time_str) {
        return false;
    }
    
    // 清空缓冲区
    ssd1306_clear(ssd1306);
    
    // 计算居中位置（假设使用2倍大小字体）
    // 时间字符串长度：5个字符（hh:mm）或8个字符（hh:mm:ss）
    // 字符宽度：5*2 = 10像素，间距：1*2 = 2像素
    // 对于hh:mm：总宽度 = 5*10 + 4*2 = 50 + 8 = 58像素，居中X = (128-58)/2 = 35
    // 对于hh:mm:ss：总宽度 = 8*10 + 7*2 = 80 + 14 = 94像素，居中X = (128-94)/2 = 17
    // 使用通用计算：根据字符串长度动态计算
    size_t len = strlen(time_str);
    uint8_t total_width = len * 10 + (len - 1) * 2;  // 假设2倍字体
    uint8_t x = (SSD1306_WIDTH - total_width) / 2;
    uint8_t y = 28;  // 垂直居中（64/2 - 7*2/2 = 32 - 7 = 25，稍微调整到28）
    
    // 绘制时间字符串（使用2倍大小）
    ssd1306_draw_string(ssd1306, x, y, time_str, 2);
    
    // 刷新到屏幕
    return ssd1306_refresh(ssd1306);
}

// 显示完整时钟界面（时间、日期、温度）
// 布局参考demo项目：顶部显示日期/星期/温度，底部显示时间
bool ssd1306_show_clock(ssd1306_t *ssd1306, const char *time_str, const char *date_str, const char *weekday_str, const char *temp_str, int8_t offset_x, int8_t offset_y) {
    if (!ssd1306 || !time_str) {
        return false;
    }
    
    // 限制偏移量范围，防止超出屏幕
    if (offset_x < -2) offset_x = -2;
    if (offset_x > 2) offset_x = 2;
    if (offset_y < -2) offset_y = -2;
    if (offset_y > 2) offset_y = 2;
    
    // 清空缓冲区
    ssd1306_clear(ssd1306);
    
    // 顶部区域：显示日期、星期和温度
    // 第1行：日期（左对齐，size=2）+ 温度（右对齐，size=1，靠右上角）
    int16_t top_y = 1 + offset_y;  // 顶部Y坐标，应用Y偏移
    if (top_y < 0) top_y = 0;
    if (top_y >= SSD1306_HEIGHT) top_y = SSD1306_HEIGHT - 1;
    uint8_t date_font_size = 2;  // 日期字体大小：2倍
    uint8_t temp_font_size = 1;   // 温度字体大小：1倍（原始大小）
    
    if (date_str) {
        // 日期左对齐，应用X偏移
        int16_t date_x = 2 + offset_x;
        if (date_x < 0) date_x = 0;
        if (date_x >= SSD1306_WIDTH) date_x = SSD1306_WIDTH - 1;
        ssd1306_draw_string(ssd1306, (uint8_t)date_x, (uint8_t)top_y, date_str, date_font_size);
    }
    
    if (temp_str) {
        // 计算温度字符串的宽度（用于右对齐，使用size=1）
        uint8_t temp_width = ssd1306_get_string_width(temp_str, temp_font_size);
        // 右对齐，靠屏幕最右上角，留1像素边距，确保不超出屏幕，应用X偏移
        int16_t temp_x = SSD1306_WIDTH - temp_width - 1 + offset_x;
        if (temp_x < 0) temp_x = 0;
        if (temp_x >= SSD1306_WIDTH) temp_x = SSD1306_WIDTH - 1;
        // 温度Y坐标向下移动20个像素，应用Y偏移
        int16_t temp_y = top_y + 20 + offset_y;
        if (temp_y < 0) temp_y = 0;
        if (temp_y >= SSD1306_HEIGHT) temp_y = SSD1306_HEIGHT - 1;
        ssd1306_draw_string(ssd1306, (uint8_t)temp_x, (uint8_t)temp_y, temp_str, temp_font_size);
    }
    
    // 第2行：星期（左对齐），使用2倍字体
    if (weekday_str) {
        int16_t weekday_y = top_y + 7 * date_font_size + 2 + offset_y;  // 日期行高度 + 间距，应用Y偏移
        if (weekday_y < 0) weekday_y = 0;
        if (weekday_y >= SSD1306_HEIGHT) weekday_y = SSD1306_HEIGHT - 1;
        int16_t weekday_x = 2 + offset_x;  // 应用X偏移
        if (weekday_x < 0) weekday_x = 0;
        if (weekday_x >= SSD1306_WIDTH) weekday_x = SSD1306_WIDTH - 1;
        ssd1306_draw_string(ssd1306, (uint8_t)weekday_x, (uint8_t)weekday_y, weekday_str, date_font_size);
    }
    
    // 底部区域：显示时间（居中，大字体）
    // 使用4倍大小字体，紧凑间距（间距为1像素，而不是4像素）
    // 时间字符串长度：5个字符（hh:mm，不显示秒）
    // 字符宽度：5*4 = 20像素，间距：1像素（紧凑布局）
    // 总宽度：5*20 + 4*1 = 100 + 4 = 104像素
    // 居中X坐标：(128 - 104) / 2 = 12
    // 计算时间显示的Y坐标：顶部区域占用约32像素（日期行14 + 间距2 + 星期行14 + 间距2）
    // 时间字体高度：7*4 = 28像素，垂直居中在剩余32像素中：32 + (32-28)/2 = 34
    int16_t x = 12 + offset_x;  // 居中显示，应用X偏移
    if (x < 0) x = 0;
    if (x >= SSD1306_WIDTH) x = SSD1306_WIDTH - 1;
    int16_t y = 34 + offset_y;  // 调整Y坐标，避免与顶部区域重叠，应用Y偏移
    if (y < 0) y = 0;
    if (y >= SSD1306_HEIGHT) y = SSD1306_HEIGHT - 1;
    
    // 绘制时间字符串（使用4倍大小，draw_string会自动使用紧凑间距）
    ssd1306_draw_string(ssd1306, (uint8_t)x, (uint8_t)y, time_str, 4);
    
    // 刷新到屏幕
    return ssd1306_refresh(ssd1306);
}

// 设置显示开关
bool ssd1306_set_display_on(ssd1306_t *ssd1306, bool on) {
    if (!ssd1306) {
        return false;
    }
    return ssd1306_write_cmd(ssd1306, on ? SSD1306_CMD_DISPLAY_ON : SSD1306_CMD_DISPLAY_OFF);
}

// 设置对比度
bool ssd1306_set_contrast(ssd1306_t *ssd1306, uint8_t contrast) {
    if (!ssd1306) {
        return false;
    }
    if (!ssd1306_write_cmd(ssd1306, SSD1306_CMD_SET_CONTRAST)) {
        return false;
    }
    return ssd1306_write_cmd(ssd1306, contrast);
}
