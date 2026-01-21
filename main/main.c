#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifndef CONFIG_LOG_MAXIMUM_LEVEL
#define CONFIG_LOG_MAXIMUM_LEVEL 5
#endif

/* Fallback for environments (IntelliSense / non-ESP-IDF builds) where CONFIG_FREERTOS_HZ
   is not provided by sdkconfig; assume 1000 Hz (1 ms tick). */
#ifndef CONFIG_FREERTOS_HZ
#define CONFIG_FREERTOS_HZ 1000
#endif

#include "esp_log.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "lwip/apps/sntp.h"
#include "ssd1306.h"
#include "ds3231.h"
#include "wifi_provisioning.h"
#include <stdint.h>
#include <time.h>

// ESP32-C3 引脚定义
// DS3231 和 SSD1306 共用 I2C 引脚
#define DS3231_SDA_PIN     GPIO_NUM_0
#define DS3231_SCL_PIN     GPIO_NUM_1

// SSD1306 I2C地址（常见为0x3C，如果不行可以尝试0x3D）
#define SSD1306_I2C_ADDR   SSD1306_I2C_ADDR_0  // 0x3C

// WiFi配置
#define WIFI_MAX_RETRY      5
#define WIFI_CONNECT_TIMEOUT_MS  15000  // WiFi连接超时时间：15秒

// NTP配置（北京时间，UTC+8）
#define NTP_SERVER1         "cn.pool.ntp.org"
#define NTP_SERVER2         "time.windows.com"
#define NTP_SERVER3         "pool.ntp.org"
#define TIMEZONE_OFFSET     8  // 北京时间 UTC+8

// NVS配置
// 注意：使用独立的命名空间 "time_sync"，与 WiFi 配网模块的 "wifi_config" 命名空间隔离
#define NVS_NAMESPACE        "time_sync"
#define NVS_KEY_LAST_SYNC    "last_sync"
#define SYNC_INTERVAL_HOURS  720  // 720小时内同步过则跳过NTP

static const char *TAG = "main";

// 全局变量
static ssd1306_t ssd1306 = {0};  // 初始化为0，确保i2c_dev为NULL
static ds3231_t ds3231;
static i2c_master_bus_handle_t i2c_bus = NULL;
static int s_retry_num = 0;
static bool sntp_synced = false;
static bool ntp_initialized = false;
static bool s_in_provisioning_mode = false;
static bool s_need_wifi_scan = false;  // 标记是否需要扫描 WiFi
static bool s_need_enter_provisioning = false;  // 标记是否需要进入配网模式
static bool s_need_ntp_sync = false;  // 标记是否需要同步NTP（全局变量，供主循环使用）
static bool s_force_ntp_sync = false;  // 标记是否需要强制NTP同步（忽略720小时限制）

// 时间结构
typedef struct {
    int hour;
    int minute;
    int second;
} Time_t;

// 从DS3231读取时间并转换为Time结构
bool readTimeFromDS3231(Time_t *time) {
    if (!time) return false;
    
    ds3231_time_t ds3231_time;
    if (ds3231_read_time(&ds3231, &ds3231_time)) {
        time->hour = ds3231_time.hours;
        time->minute = ds3231_time.minutes;
        time->second = ds3231_time.seconds;
        return true;
    }
    return false;
}

// 将Time结构转换为ds3231_time_t并写入DS3231（只更新时间部分，保留日期）
bool writeTimeToDS3231(const Time_t *time) {
    if (!time) return false;
    
    // 先读取当前完整时间（包括日期）
    ds3231_time_t ds3231_time;
    if (!ds3231_read_time(&ds3231, &ds3231_time)) {
        return false;
    }
    
    // 更新时间部分
    ds3231_time.hours = time->hour;
    ds3231_time.minutes = time->minute;
    ds3231_time.seconds = time->second;
    
    // 写入DS3231
    return ds3231_write_time(&ds3231, &ds3231_time);
}

// Display time to SSD1306 (with date, weekday and temperature)
void displayTime(const Time_t *time) {
    if (!time) return;
    
    // 只有在SSD1306初始化成功时才显示
    if (ssd1306.i2c_dev == NULL) {
        return;
    }
    
    // 根据时间段自动调整亮度
    // 晚上（18:00-23:59）：亮度75%（0xCF * 0.75 ≈ 0x9B）
    // 白天（06:00-17:59）：亮度100%（0xCF）
    // 夜间（00:00-05:59）：亮度75%（0xCF * 0.75 ≈ 0x9B）
    static uint8_t last_brightness = 0;
    uint8_t target_brightness;
    if (time->hour >= 18 || time->hour < 6) {
        target_brightness = 0x9B;  // 75%亮度（0xCF * 0.75 ≈ 0x9B）
    } else {
        target_brightness = 0xCF;  // 100%亮度（原始值）
    }
    
    // 只在亮度需要改变时更新
    if (last_brightness != target_brightness) {
        ssd1306_set_contrast(&ssd1306, target_brightness);
        last_brightness = target_brightness;
        ESP_LOGD(TAG, "Brightness adjusted to %d%% (%02X) for hour %02d", 
                 time->hour >= 18 || time->hour < 6 ? 75 : 100, target_brightness, time->hour);
    }
    
    // 像素位移防烧屏：每5分钟轻微移动显示位置
    // 使用分钟数计算位移，循环移动：0->1->2->1->0->-1->-2->-1->0...
    static int8_t last_offset_x = 0;
    static int8_t last_offset_y = 0;
    static int last_cycle = -1;
    int8_t offset_x = 0;
    int8_t offset_y = 0;
    int cycle = (time->minute / 5) % 8;  // 每5分钟一个周期，8个位置循环
    switch (cycle) {
        case 0: offset_x = 0;  offset_y = 0;  break;   // 中心
        case 1: offset_x = 1;  offset_y = 0;  break;   // 右
        case 2: offset_x = 1;  offset_y = 1;  break;   // 右下
        case 3: offset_x = 0;  offset_y = 1;  break;   // 下
        case 4: offset_x = -1; offset_y = 1;  break;   // 左下
        case 5: offset_x = -1; offset_y = 0;  break;   // 左
        case 6: offset_x = -1; offset_y = -1; break;   // 左上
        case 7: offset_x = 0;  offset_y = -1; break;   // 上
        default: offset_x = 0; offset_y = 0; break;
    }
    
    // 当像素位移改变时输出日志（每5分钟改变一次）
    if (cycle != last_cycle || offset_x != last_offset_x || offset_y != last_offset_y) {
        const char* position_names[] = {"中心", "右", "右下", "下", "左下", "左", "左上", "上"};
        ESP_LOGI(TAG, "Pixel shift changed: cycle=%d, offset=(%d, %d), position=%s", 
                 cycle, offset_x, offset_y, position_names[cycle]);
        last_offset_x = offset_x;
        last_offset_y = offset_y;
        last_cycle = cycle;
    }
    
    // 读取完整的DS3231时间（包括日期）
    ds3231_time_t ds3231_time;
    if (!ds3231_read_time(&ds3231, &ds3231_time)) {
        // 如果读取失败，只显示时间（冒号闪烁）
        char timeStr[6];
        if (time->second % 2 == 0) {
            snprintf(timeStr, sizeof(timeStr), "%02d:%02d", 
                     time->hour, time->minute);
        } else {
            snprintf(timeStr, sizeof(timeStr), "%02d %02d", 
                     time->hour, time->minute);
        }
        ssd1306_show_time(&ssd1306, timeStr);
        return;
    }
    
    // 格式化时间字符串（只显示HH:MM，不显示秒）
    // 冒号根据秒数奇偶性闪烁：偶数秒显示冒号，奇数秒显示空格
    char timeStr[6];
    if (time->second % 2 == 0) {
        // 偶数秒：显示冒号
        snprintf(timeStr, sizeof(timeStr), "%02d:%02d", 
                 time->hour, time->minute);
    } else {
        // 奇数秒：冒号位置显示空格（实现闪烁效果）
        snprintf(timeStr, sizeof(timeStr), "%02d %02d", 
                 time->hour, time->minute);
    }
    
    // 格式化日期字符串
    char dateStr[16];
    snprintf(dateStr, sizeof(dateStr), "%04d-%02d-%02d", 
             2000 + ds3231_time.year, ds3231_time.month, ds3231_time.date);
    
    // 格式化星期字符串（DS3231的day: 1=Sunday, 2=Monday, ..., 7=Saturday）
    const char* weekdays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    const char* weekdayStr = (ds3231_time.day >= 1 && ds3231_time.day <= 7) ? 
                             weekdays[ds3231_time.day - 1] : "---";
    
    // 读取温度
    float temperature = 0.0f;
    char tempStr[12] = "---c";
    if (ds3231_read_temperature(&ds3231, &temperature)) {
        snprintf(tempStr, sizeof(tempStr), "%.1fc", temperature);
    }
    
    // 显示完整时钟界面（带像素位移）
    ssd1306_show_clock(&ssd1306, timeStr, dateStr, weekdayStr, tempStr, offset_x, offset_y);
}

// Parse time string in format "hh:mm:ss"
bool parseTimeString(const char *str, Time_t *time) {
    if (!str || !time) return false;
    
    int h, m, s;
    if (sscanf(str, "%d:%d:%d", &h, &m, &s) == 3) {
        // Validate time values
        if (h >= 0 && h < 24 && m >= 0 && m < 60 && s >= 0 && s < 60) {
            time->hour = h;
            time->minute = m;
            time->second = s;
            return true;
        }
    }
    return false;
}

// NVS: 保存上次同步时间戳
static void save_last_sync_time(time_t sync_time)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(err));
        return;
    }
    
    err = nvs_set_i64(nvs_handle, NVS_KEY_LAST_SYNC, (int64_t)sync_time);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving last sync time: %s", esp_err_to_name(err));
    } else {
        nvs_commit(nvs_handle);
        ESP_LOGI(TAG, "Last sync time saved to NVS: %lld", (long long)sync_time);
    }
    
    nvs_close(nvs_handle);
}

// NVS: 读取上次同步时间戳
static time_t get_last_sync_time(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "NVS namespace not found or error opening: %s", esp_err_to_name(err));
        return 0;
    }
    
    int64_t last_sync = 0;
    err = nvs_get_i64(nvs_handle, NVS_KEY_LAST_SYNC, &last_sync);
    nvs_close(nvs_handle);
    
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "Last sync time not found in NVS: %s", esp_err_to_name(err));
        return 0;
    }
    
    ESP_LOGI(TAG, "Last sync time from NVS: %lld", (long long)last_sync);
    return (time_t)last_sync;
}

// 检查是否需要同步NTP（720小时内同步过则返回false）
static bool should_sync_ntp(void)
{
    // 如果强制同步标志被设置，直接返回true（忽略720小时限制）
    // 使用静态变量避免重复输出日志
    static bool force_sync_logged = false;
    if (s_force_ntp_sync) {
        if (!force_sync_logged) {
            ESP_LOGI(TAG, "Force NTP sync requested");
            force_sync_logged = true;
        }
        return true;
    } else {
        // 如果强制同步标志被清除，重置日志标志
        force_sync_logged = false;
    }
    
    time_t last_sync = get_last_sync_time();
    if (last_sync == 0) {
        ESP_LOGI(TAG, "No previous sync record found, will sync NTP");
        return true;
    }
    
    // 尝试使用系统时间，如果不可用则使用DS3231时间
    time_t now = time(NULL);
    if (now <= 0) {
        // 系统时间不可用，尝试从DS3231获取时间
        ds3231_time_t ds3231_time;
        if (ds3231_read_time(&ds3231, &ds3231_time)) {
            // 将DS3231时间转换为time_t（简化计算，仅用于判断时间差）
            struct tm tm_time = {0};
            tm_time.tm_sec = ds3231_time.seconds;
            tm_time.tm_min = ds3231_time.minutes;
            tm_time.tm_hour = ds3231_time.hours;
            tm_time.tm_mday = ds3231_time.date;
            tm_time.tm_mon = ds3231_time.month - 1;
            tm_time.tm_year = 2000 + ds3231_time.year - 1900;
            now = mktime(&tm_time);
            
            if (now > 0) {
                ESP_LOGI(TAG, "Using DS3231 time to check sync interval");
            } else {
                ESP_LOGW(TAG, "Cannot get time from DS3231, will sync NTP");
                return true;
            }
        } else {
            ESP_LOGW(TAG, "Cannot read DS3231 time, will sync NTP");
            return true;
        }
    }
    
    // 计算时间差（秒）
    time_t diff = now - last_sync;
    time_t interval_seconds = SYNC_INTERVAL_HOURS * 3600;
    
    if (diff < 0 || diff >= interval_seconds) {
        ESP_LOGI(TAG, "Last sync was %lld seconds ago (>= %d hours), will sync NTP", 
                 (long long)diff, SYNC_INTERVAL_HOURS);
        return true;
    }
    
    ESP_LOGI(TAG, "Last sync was %lld seconds ago (< %d hours), skipping NTP sync", 
             (long long)diff, SYNC_INTERVAL_HOURS);
    return false;
}

// WiFi 连接状态回调
static void wifi_status_callback(bool connected, const char* ip)
{
    if (connected) {
        // 注意：wifi_provisioning.c 中已经输出了 "Got IP" 日志
        // 这里只输出一次连接成功日志，避免重复
        ESP_LOGI(TAG, "WiFi connected successfully! IP: %s", ip ? ip : "unknown");
        s_retry_num = 0;
        
        // 如果之前在配网模式，现在连接成功，停止配网模式
        if (s_in_provisioning_mode) {
            ESP_LOGI(TAG, "Stopping provisioning mode");
            wifi_provisioning_stop_softap();
            s_in_provisioning_mode = false;
        }
    } else {
        ESP_LOGI(TAG, "WiFi disconnected");
    }
}

// WiFi事件处理（用于重试逻辑和状态回调）
// 注意：WIFI_EVENT_STA_START 事件已在 wifi_provisioning.c 中处理，这里不再重复处理
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                              int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t* event = (wifi_event_sta_disconnected_t*) event_data;
        // 简化日志输出，避免栈溢出
        ESP_LOGW(TAG, "WiFi disconnected, reason: %d", event->reason);
        
        // 如果是 "No AP found" 错误，标记需要扫描（在主循环中执行，避免栈溢出）
        if (event->reason == WIFI_REASON_NO_AP_FOUND && s_retry_num == 0) {
            s_need_wifi_scan = true;
        }
        
        if (s_retry_num < WIFI_MAX_RETRY) {
            // 注意：在事件处理函数中使用 vTaskDelay 会阻塞事件循环
            // 但这是必要的，因为我们需要延迟重试
            // 等待15秒后重试连接
            ESP_LOGI(TAG, "Waiting %d seconds before retry (%d/%d)...", 
                     WIFI_CONNECT_TIMEOUT_MS / 1000, s_retry_num + 1, WIFI_MAX_RETRY);
            vTaskDelay(pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));
            esp_err_t ret = esp_wifi_connect();
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "esp_wifi_connect() failed: %s", esp_err_to_name(ret));
            }
            s_retry_num++;
            ESP_LOGI(TAG, "Retry to connect (%d/%d)", s_retry_num, WIFI_MAX_RETRY);
        } else {
            ESP_LOGE(TAG, "Failed to connect after %d retries", WIFI_MAX_RETRY);
            ESP_LOGI(TAG, "All connection attempts failed. Will enter provisioning mode.");
            // 标记需要进入配网模式（在主循环中处理，避免在事件处理函数中执行复杂操作）
            s_need_enter_provisioning = true;
            wifi_status_callback(false, NULL);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        // 注意：wifi_provisioning.c 中已经处理了 IP_EVENT_STA_GOT_IP 并调用了回调
        // 这里不再重复调用，避免重复日志
        // wifi_provisioning.c 中的回调已经足够
    }
}

// 执行 WiFi 扫描（独立函数，避免栈溢出）
static void perform_wifi_scan(void)
{
    ESP_LOGI(TAG, "Scanning for available WiFi networks...");
    wifi_scan_config_t scan_config = {
        .ssid = NULL,  // 扫描所有网络
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,  // 包括隐藏的 SSID
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = {
            .active = {
                .min = 100,
                .max = 300,
            }
        }
    };
    
    esp_err_t ret = esp_wifi_scan_start(&scan_config, true);  // 阻塞扫描
    if (ret == ESP_OK) {
        uint16_t ap_count = 0;
        esp_wifi_scan_get_ap_num(&ap_count);
        ESP_LOGI(TAG, "Found %d WiFi networks:", ap_count);
        
        if (ap_count > 0) {
            // 减少数组大小，避免栈溢出（最多显示10个）
            wifi_ap_record_t ap_records[10];
            uint16_t ap_records_count = ap_count > 10 ? 10 : ap_count;
            esp_wifi_scan_get_ap_records(&ap_records_count, ap_records);
            
            // 读取配置的 SSID
            wifi_config_data_t wifi_config_data;
            const char* target_ssid = NULL;
            if (wifi_provisioning_load_config(&wifi_config_data) == ESP_OK) {
                target_ssid = wifi_config_data.ssid;
            }
            
            bool found_target = false;
            for (int i = 0; i < ap_records_count; i++) {
                const char* match = "";
                if (target_ssid && strcmp((char*)ap_records[i].ssid, target_ssid) == 0) {
                    match = " <-- TARGET";
                    found_target = true;
                }
                ESP_LOGI(TAG, "  [%d] SSID: %s, RSSI: %d dBm, Auth: %d%s",
                        i + 1, ap_records[i].ssid, ap_records[i].rssi, 
                        ap_records[i].authmode, match);
            }
            
            if (target_ssid && !found_target) {
                ESP_LOGW(TAG, "Target SSID '%s' not found!", target_ssid);
                ESP_LOGW(TAG, "Check: router power, SSID spelling, 2.4GHz band, MAC filter");
            }
        } else {
            ESP_LOGW(TAG, "No WiFi networks found.");
        }
    } else {
        ESP_LOGW(TAG, "WiFi scan failed: %s", esp_err_to_name(ret));
    }
}

// 初始化WiFi Station模式
static esp_err_t wifi_init_sta(void)
{
    // 从 NVS 读取 WiFi 配置
    wifi_config_data_t wifi_config_data;
    esp_err_t ret = wifi_provisioning_load_config(&wifi_config_data);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load WiFi config from NVS: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "Initializing WiFi Station mode...");
    ESP_LOGI(TAG, "Connecting to SSID: %s", wifi_config_data.ssid);
    
    // 启动 WiFi Station
    ret = wifi_provisioning_start_sta(&wifi_config_data, wifi_status_callback);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WiFi Station: %s", esp_err_to_name(ret));
        return ret;
    }
    
    return ESP_OK;
}

// 关闭WiFi模块以节省功耗（已废弃，使用 wifi_provisioning_stop_softap 代替）
static void wifi_deinit_sta(void)
{
    ESP_LOGI(TAG, "Deinitializing WiFi to save power...");
    wifi_provisioning_stop_softap();
}

// 检查并同步NTP时间到DS3231
static void sync_ntp_to_ds3231(void)
{
    if (sntp_synced) {
        return;  // 已经同步过了
    }
    
    // 检查SNTP是否已同步
    time_t now = 0;
    struct tm timeinfo = {0};
    time(&now);
    
    if (now > 0) {
        localtime_r(&now, &timeinfo);
        
        // 检查时间是否合理（年份应该在2020-2099之间）
        if (timeinfo.tm_year >= 120 && timeinfo.tm_year < 200) {
            ESP_LOGI(TAG, "SNTP time synchronized!");
            sntp_synced = true;
            
            // 如果是强制同步，清除标志
            if (s_force_ntp_sync) {
                ESP_LOGI(TAG, "Force NTP sync completed successfully");
                s_force_ntp_sync = false;
                // 注意：force_sync_logged 会在下次 should_sync_ntp() 调用时自动重置
            }
            
            // 将NTP时间同步到DS3231
            ds3231_time_t ds3231_time;
            ds3231_time.seconds = timeinfo.tm_sec;
            ds3231_time.minutes = timeinfo.tm_min;
            ds3231_time.hours = timeinfo.tm_hour;
            ds3231_time.day = timeinfo.tm_wday == 0 ? 7 : timeinfo.tm_wday;  // 转换为1-7（周日=7）
            ds3231_time.date = timeinfo.tm_mday;
            ds3231_time.month = timeinfo.tm_mon + 1;  // tm_mon是0-11
            ds3231_time.year = timeinfo.tm_year - 100;  // tm_year是1900年起的年数，转换为0-99
            
            if (ds3231_write_time(&ds3231, &ds3231_time)) {
                ESP_LOGI(TAG, "Time synchronized to DS3231: %04d-%02d-%02d %02d:%02d:%02d",
                         2000 + ds3231_time.year, ds3231_time.month, ds3231_time.date,
                         ds3231_time.hours, ds3231_time.minutes, ds3231_time.seconds);
                
                // 保存同步时间戳到NVS
                save_last_sync_time(now);
                
                // 同步完成后关闭WiFi以节省功耗
                wifi_deinit_sta();
                ntp_initialized = false;  // 标记不再需要检查NTP同步
            } else {
                ESP_LOGE(TAG, "Failed to write time to DS3231");
            }
        }
    }
}

// 初始化SNTP
static void sntp_init_func(void)
{
    ESP_LOGI(TAG, "Initializing SNTP...");
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    
    // 设置NTP服务器（北京时间）
    sntp_setservername(0, NTP_SERVER1);
    sntp_setservername(1, NTP_SERVER2);
    sntp_setservername(2, NTP_SERVER3);
    
    // 设置时区（北京时间 UTC+8）
    // 注意：时区已在 app_main() 开始时设置，这里再次设置以确保正确性
    setenv("TZ", "CST-8", 1);
    tzset();
    
    // 启动SNTP
    sntp_init();
    
    ESP_LOGI(TAG, "SNTP initialized. Waiting for time sync...");
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== NTP Timer with DS3231 and SSD1306 ===");
    
    // 初始化NVS（用于存储同步时间戳）
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // NVS分区被占用或版本不匹配，擦除并重新初始化
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    // 设置时区（北京时间 UTC+8）- 必须在调用 should_sync_ntp() 之前设置
    // 这样 mktime() 才能正确将 DS3231 的本地时间转换为 UTC 时间戳
    setenv("TZ", "CST-8", 1);
    tzset();
    ESP_LOGI(TAG, "Timezone set to CST-8 (UTC+8)");
    
    // 初始化I2C总线（DS3231和SSD1306共用）
    ESP_LOGI(TAG, "Initializing I2C bus...");
    i2c_master_bus_config_t i2c_bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = DS3231_SDA_PIN,
        .scl_io_num = DS3231_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = {
            .enable_internal_pullup = true,
        },
    };
    
    ret = i2c_new_master_bus(&i2c_bus_config, &i2c_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize I2C bus: %s", esp_err_to_name(ret));
        return;
    }
    
    // 初始化DS3231
    ESP_LOGI(TAG, "Initializing DS3231 RTC...");
    if (!ds3231_init(&ds3231, i2c_bus, DS3231_SDA_PIN, DS3231_SCL_PIN)) {
        ESP_LOGE(TAG, "Failed to initialize DS3231!");
        ESP_LOGE(TAG, "Please check I2C connections (SDA=GPIO%d, SCL=GPIO%d)", DS3231_SDA_PIN, DS3231_SCL_PIN);
    } else {
        ESP_LOGI(TAG, "DS3231 initialized successfully");
        
        // 检查振荡器状态
        bool oscillator_stopped = false;
        if (ds3231_is_oscillator_stopped(&ds3231, &oscillator_stopped)) {
            if (oscillator_stopped) {
                ESP_LOGW(TAG, "Warning: DS3231 oscillator was stopped. Time may be inaccurate.");
            }
        }
    }
    
    // 初始化SSD1306显示模块（与DS3231共用I2C总线）
    ESP_LOGI(TAG, "Initializing SSD1306 display...");
    // 尝试两个常见的I2C地址
    bool ssd1306_ok = false;
    if (ssd1306_init(&ssd1306, i2c_bus, SSD1306_I2C_ADDR_0)) {
        ESP_LOGI(TAG, "SSD1306 initialized successfully at address 0x%02X", SSD1306_I2C_ADDR_0);
        ssd1306_ok = true;
    } else {
        ESP_LOGW(TAG, "Failed to initialize SSD1306 at address 0x%02X, trying 0x%02X...", 
                 SSD1306_I2C_ADDR_0, SSD1306_I2C_ADDR_1);
        // 如果第一个地址失败，尝试第二个地址
        if (ssd1306_init(&ssd1306, i2c_bus, SSD1306_I2C_ADDR_1)) {
            ESP_LOGI(TAG, "SSD1306 initialized successfully at address 0x%02X", SSD1306_I2C_ADDR_1);
            ssd1306_ok = true;
        } else {
            ESP_LOGE(TAG, "Failed to initialize SSD1306 at both addresses (0x%02X and 0x%02X)", 
                     SSD1306_I2C_ADDR_0, SSD1306_I2C_ADDR_1);
            ESP_LOGE(TAG, "Please check I2C connections (SDA=GPIO%d, SCL=GPIO%d)", 
                     DS3231_SDA_PIN, DS3231_SCL_PIN);
            ESP_LOGW(TAG, "System will continue without display");
        }
    }
    
    // 从DS3231读取时间并显示
    Time_t currentTime = {15, 29, 15};  // 默认时间
    if (readTimeFromDS3231(&currentTime)) {
        ESP_LOGI(TAG, "Time read from DS3231: %02d:%02d:%02d", 
               currentTime.hour, currentTime.minute, currentTime.second);
    } else {
        ESP_LOGW(TAG, "Failed to read time from DS3231, using default time: %02d:%02d:%02d",
                currentTime.hour, currentTime.minute, currentTime.second);
    }
    
    // 显示初始时间
    displayTime(&currentTime);
    
    // 初始化 WiFi 配网模块（内部已注册事件处理器）
    ESP_LOGI(TAG, "Initializing WiFi provisioning module...");
    ESP_ERROR_CHECK(wifi_provisioning_init());
    
    // 注册额外的 WiFi 事件处理器（用于 Station 模式连接状态和重试逻辑）
    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));
    
    // 优先检查是否有保存的 WiFi 配置
    // 如果没有配置，直接进入配网模式
    ESP_LOGI(TAG, "Checking for saved WiFi config in NVS...");
    bool has_wifi_config = wifi_provisioning_has_config();
    
    if (!has_wifi_config) {
        // 没有WiFi配置，自动启动 SoftAP 配网模式
        ESP_LOGI(TAG, "No WiFi config found in NVS. Automatically entering provisioning mode...");
        ESP_LOGI(TAG, "Please connect to WiFi hotspot 'PIX_Clock_Setup' and open http://192.168.4.1");
        s_in_provisioning_mode = true;
        ESP_ERROR_CHECK(wifi_provisioning_start_softap(wifi_status_callback));
    } else {
        // 有WiFi配置，检查是否需要同步NTP（决定是否需要启动WiFi）
        s_need_ntp_sync = should_sync_ntp();
        ESP_LOGI(TAG, "WiFi config found. NTP sync check: %s", s_need_ntp_sync ? "needed" : "not needed");
        
        if (s_need_ntp_sync) {
        // 有配置且需要NTP同步，启动WiFi连接
        ESP_LOGI(TAG, "WiFi config found. NTP sync needed. Connecting to WiFi...");
        esp_err_t ret = wifi_init_sta();
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to start WiFi Station, entering provisioning mode");
                s_in_provisioning_mode = true;
                ESP_ERROR_CHECK(wifi_provisioning_start_softap(wifi_status_callback));
            } else {
                // WiFi启动成功，等待连接后初始化SNTP
                ESP_LOGI(TAG, "NTP sync needed. Waiting for WiFi connection...");
                esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
                esp_netif_ip_info_t ip_info;
                int retry_count = 0;
                bool wifi_connected = false;
                
                while (retry_count < 30) {  // 最多等待30秒
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    retry_count++;
                    if (sta_netif && esp_netif_get_ip_info(sta_netif, &ip_info) == ESP_OK) {
                        if (ip_info.ip.addr != 0) {
                            wifi_connected = true;
                            ESP_LOGI(TAG, "WiFi connected, initializing SNTP...");
                            break;
                        }
                    }
                }
                
                if (!wifi_connected) {
                    ESP_LOGW(TAG, "WiFi connection failed after 30 seconds. Entering provisioning mode.");
                    // WiFi连接失败，进入配网模式
                    wifi_provisioning_stop_softap();  // 先停止Station模式
                    vTaskDelay(pdMS_TO_TICKS(500));  // 等待WiFi完全停止
                    s_retry_num = 0;  // 重置重试计数
                    s_need_enter_provisioning = true;  // 标记需要进入配网模式
                    ntp_initialized = false;
                } else {
                    // WiFi连接成功，初始化SNTP
                    sntp_init_func();
                    ntp_initialized = true;
                }
            }
        } else {
            // 有配置但不需要NTP同步，不启动WiFi以节省功耗
            ESP_LOGI(TAG, "WiFi config found but NTP sync not needed (last sync was within %d hours).", SYNC_INTERVAL_HOURS);
            ESP_LOGI(TAG, "Skipping WiFi initialization to save power. Using DS3231 time directly.");
            ntp_initialized = false;
            // 不启动WiFi，直接使用DS3231时间
        }
    }
    
    if (s_in_provisioning_mode) {
        ESP_LOGI(TAG, "In provisioning mode, NTP sync will be performed after WiFi is configured.");
        ntp_initialized = false;
    }
    
    ESP_LOGI(TAG, "System ready. Time will update every second.");
    
    // Main loop: read time from DS3231 every second
    TickType_t lastUpdate = xTaskGetTickCount();
    TickType_t lastNtpCheck = xTaskGetTickCount();
    TickType_t lastProvCheck = xTaskGetTickCount();  // 配网模式检查
    TickType_t ntpSyncStartTime = xTaskGetTickCount();  // 记录NTP同步开始时间
    const TickType_t updateIntervalMs = pdMS_TO_TICKS(1000);  // 1 second
    const TickType_t ntpCheckIntervalMs = pdMS_TO_TICKS(5000);  // 每5秒检查一次NTP同步
    const TickType_t provCheckIntervalMs = pdMS_TO_TICKS(2000);  // 每2秒检查一次配网状态
    const TickType_t ntpSyncTimeoutMs = pdMS_TO_TICKS(60000);  // NTP同步超时时间：60秒
    
    while (1) {
        TickType_t now = xTaskGetTickCount();
        
        // 如果需要扫描 WiFi（在主循环中执行，避免事件处理函数栈溢出）
        if (s_need_wifi_scan) {
            s_need_wifi_scan = false;
            // 将扫描操作移到独立函数，避免在主循环中使用大数组
            perform_wifi_scan();
        }
        
        // 如果5次重试都失败，进入配网模式
        if (s_need_enter_provisioning) {
            s_need_enter_provisioning = false;
            ESP_LOGI(TAG, "Entering provisioning mode due to connection failure...");
            
            // 停止当前的 WiFi（无论是 Station 还是 SoftAP 模式）
            esp_err_t ret = esp_wifi_stop();
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Failed to stop WiFi: %s", esp_err_to_name(ret));
            }
            vTaskDelay(pdMS_TO_TICKS(500));  // 等待 WiFi 完全停止
            
            // 清除旧的无效配置，避免配网模式启动后立即检测到旧配置
            ESP_LOGI(TAG, "Clearing invalid WiFi config...");
            wifi_provisioning_clear_config();
            
            // 重置重试计数
            s_retry_num = 0;
            
            // 启动 SoftAP 配网模式
            s_in_provisioning_mode = true;
            ret = wifi_provisioning_start_softap(wifi_status_callback);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to start provisioning mode: %s", esp_err_to_name(ret));
            } else {
                ESP_LOGI(TAG, "Provisioning mode started. Connect to 'PIX_Clock_Setup' and visit http://192.168.4.1");
            }
        }
        
        // 在配网模式下，定期检查是否有新配置保存
        if (s_in_provisioning_mode && (now - lastProvCheck) >= provCheckIntervalMs) {
            lastProvCheck = now;
            if (wifi_provisioning_has_config()) {
                // 检测到新配置，停止配网模式并连接 WiFi
                ESP_LOGI(TAG, "WiFi config detected, stopping provisioning and connecting...");
                wifi_provisioning_stop_softap();
                s_in_provisioning_mode = false;
                
                // 启动 Station 模式
                esp_err_t ret = wifi_init_sta();
                if (ret == ESP_OK) {
                    // 注意：不在这里初始化NTP，因为WiFi可能还没有连接成功
                    // 主循环会检测到WiFi连接并初始化SNTP（见第710-722行的逻辑）
                }
            }
        }
        
        // 定期检查NTP同步状态（每5秒，仅在NTP已初始化时）
        if (ntp_initialized && (now - lastNtpCheck) >= ntpCheckIntervalMs) {
            sync_ntp_to_ds3231();
            lastNtpCheck = now;
            
            // 检查是否超时（60秒内未同步成功）
            if (!sntp_synced && (now - ntpSyncStartTime) >= ntpSyncTimeoutMs) {
                ESP_LOGW(TAG, "NTP sync timeout after 60 seconds. Closing WiFi to save power.");
                wifi_provisioning_stop_softap();
                ntp_initialized = false;
            }
        }
        
        // 如果 WiFi 已连接且需要同步 NTP，但 NTP 还未初始化，则初始化 NTP
        // 注意：如果s_force_ntp_sync为true，需要重新检查should_sync_ntp()来更新s_need_ntp_sync
        // 但只在WiFi已连接时才检查，避免重复调用should_sync_ntp()
        if (s_force_ntp_sync && !ntp_initialized) {
            esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            esp_netif_ip_info_t ip_info;
            // 只有在WiFi已连接时才更新s_need_ntp_sync，避免重复日志
            if (sta_netif && esp_netif_get_ip_info(sta_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
                s_need_ntp_sync = should_sync_ntp();  // 重新检查，此时should_sync_ntp会返回true（只输出一次日志）
            }
        }
        
        if (!s_in_provisioning_mode && !ntp_initialized && s_need_ntp_sync) {
            esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            esp_netif_ip_info_t ip_info;
            if (sta_netif && esp_netif_get_ip_info(sta_netif, &ip_info) == ESP_OK) {
                if (ip_info.ip.addr != 0) {
                    ESP_LOGI(TAG, "WiFi connected, initializing SNTP...");
                    if (s_force_ntp_sync) {
                        ESP_LOGI(TAG, "Force NTP sync mode");
                    }
                    sntp_init_func();
                    ntp_initialized = true;
                    ntpSyncStartTime = now;
                    ESP_LOGI(TAG, "SNTP initialization started. Waiting for time sync...");
                }
            }
        }
        
        // Check if 1 second has passed
        if ((now - lastUpdate) >= updateIntervalMs) {
            // 从DS3231读取最新时间
            if (!readTimeFromDS3231(&currentTime)) {
                // 如果读取失败，使用软件计时（向后兼容）
                currentTime.second++;
                if (currentTime.second >= 60) {
                    currentTime.second = 0;
                    currentTime.minute++;
                    if (currentTime.minute >= 60) {
                        currentTime.minute = 0;
                        currentTime.hour++;
                        if (currentTime.hour >= 24) {
                            currentTime.hour = 0;
                        }
                    }
                }
            }
            displayTime(&currentTime);
            lastUpdate = now;
        }
        
        // Small delay to prevent CPU spinning
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
