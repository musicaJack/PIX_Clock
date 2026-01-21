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

// ESP32-C3 pin definitions
// DS3231 and SSD1306 share I2C pins
#define DS3231_SDA_PIN     GPIO_NUM_0
#define DS3231_SCL_PIN     GPIO_NUM_1

// SSD1306 I2C address (common is 0x3C, try 0x3D if it doesn't work)
#define SSD1306_I2C_ADDR   SSD1306_I2C_ADDR_0  // 0x3C

// WiFi configuration
#define WIFI_MAX_RETRY      5
#define WIFI_CONNECT_TIMEOUT_MS  15000  // WiFi connection timeout: 15 seconds

// NTP configuration (Beijing time, UTC+8)
#define NTP_SERVER1         "cn.pool.ntp.org"
#define NTP_SERVER2         "time.windows.com"
#define NTP_SERVER3         "pool.ntp.org"
#define TIMEZONE_OFFSET     8  // Beijing time UTC+8

// NVS configuration
// Note: Use independent namespace "time_sync", isolated from WiFi provisioning module's "wifi_config" namespace
#define NVS_NAMESPACE        "time_sync"
#define NVS_KEY_LAST_SYNC    "last_sync"
#define SYNC_INTERVAL_HOURS  720  // Skip NTP sync if synced within 720 hours

static const char *TAG = "main";

// Global variables
static ssd1306_t ssd1306 = {0};  // Initialize to 0, ensure i2c_dev is NULL
static ds3231_t ds3231;
static i2c_master_bus_handle_t i2c_bus = NULL;
static int s_retry_num = 0;
static bool sntp_synced = false;
static bool ntp_initialized = false;
static bool s_in_provisioning_mode = false;
static bool s_need_wifi_scan = false;  // Flag to indicate if WiFi scan is needed
static bool s_need_enter_provisioning = false;  // Flag to indicate if provisioning mode is needed
static bool s_need_ntp_sync = false;  // Flag to indicate if NTP sync is needed (global variable for main loop)
static bool s_force_ntp_sync = false;  // Flag to indicate if forced NTP sync is needed (ignore 720-hour limit)

// Time structure
typedef struct {
    int hour;
    int minute;
    int second;
} Time_t;

// Read time from DS3231 and convert to Time structure
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

// Convert Time structure to ds3231_time_t and write to DS3231 (only update time part, preserve date)
bool writeTimeToDS3231(const Time_t *time) {
    if (!time) return false;
    
    // First read current complete time (including date)
    ds3231_time_t ds3231_time;
    if (!ds3231_read_time(&ds3231, &ds3231_time)) {
        return false;
    }
    
    // Update time part
    ds3231_time.hours = time->hour;
    ds3231_time.minutes = time->minute;
    ds3231_time.seconds = time->second;
    
    // Write to DS3231
    return ds3231_write_time(&ds3231, &ds3231_time);
}

// Display time to SSD1306 (with date, weekday and temperature)
void displayTime(const Time_t *time) {
    if (!time) return;
    
    // Only display if SSD1306 is initialized successfully
    if (ssd1306.i2c_dev == NULL) {
        return;
    }
    
    // Automatically adjust brightness based on time period
    // Night (18:00-23:59): 75% brightness (0xCF * 0.75 ≈ 0x9B)
    // Daytime (06:00-17:59): 100% brightness (0xCF)
    // Night (00:00-05:59): 75% brightness (0xCF * 0.75 ≈ 0x9B)
    static uint8_t last_brightness = 0;
    uint8_t target_brightness;
    if (time->hour >= 18 || time->hour < 6) {
        target_brightness = 0x9B;  // 75% brightness (0xCF * 0.75 ≈ 0x9B)
    } else {
        target_brightness = 0xCF;  // 100% brightness (original value)
    }
    
    // Only update when brightness needs to change
    if (last_brightness != target_brightness) {
        ssd1306_set_contrast(&ssd1306, target_brightness);
        last_brightness = target_brightness;
        ESP_LOGD(TAG, "Brightness adjusted to %d%% (%02X) for hour %02d", 
                 time->hour >= 18 || time->hour < 6 ? 75 : 100, target_brightness, time->hour);
    }
    
    // Pixel shift to prevent burn-in: slightly move display position every 5 minutes
    // Use minute value to calculate offset, cycle movement: 0->1->2->1->0->-1->-2->-1->0...
    static int8_t last_offset_x = 0;
    static int8_t last_offset_y = 0;
    static int last_cycle = -1;
    int8_t offset_x = 0;
    int8_t offset_y = 0;
    int cycle = (time->minute / 5) % 8;  // One cycle every 5 minutes, 8 positions cycle
    switch (cycle) {
        case 0: offset_x = 0;  offset_y = 0;  break;   // Center
        case 1: offset_x = 1;  offset_y = 0;  break;   // Right
        case 2: offset_x = 1;  offset_y = 1;  break;   // Bottom-right
        case 3: offset_x = 0;  offset_y = 1;  break;   // Bottom
        case 4: offset_x = -1; offset_y = 1;  break;   // Bottom-left
        case 5: offset_x = -1; offset_y = 0;  break;   // Left
        case 6: offset_x = -1; offset_y = -1; break;   // Top-left
        case 7: offset_x = 0;  offset_y = -1; break;   // Top
        default: offset_x = 0; offset_y = 0; break;
    }
    
    // Output log when pixel shift changes (changes every 5 minutes)
    if (cycle != last_cycle || offset_x != last_offset_x || offset_y != last_offset_y) {
        const char* position_names[] = {"Center", "Right", "Bottom-right", "Bottom", "Bottom-left", "Left", "Top-left", "Top"};
        ESP_LOGI(TAG, "Pixel shift changed: cycle=%d, offset=(%d, %d), position=%s", 
                 cycle, offset_x, offset_y, position_names[cycle]);
        last_offset_x = offset_x;
        last_offset_y = offset_y;
        last_cycle = cycle;
    }
    
    // Read complete DS3231 time (including date)
    ds3231_time_t ds3231_time;
    if (!ds3231_read_time(&ds3231, &ds3231_time)) {
        // If read fails, only display time (colon blinking)
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
    
    // Format time string (only display HH:MM, not seconds)
    // Colon blinks based on second parity: even seconds show colon, odd seconds show space
    char timeStr[6];
    if (time->second % 2 == 0) {
        // Even seconds: show colon
        snprintf(timeStr, sizeof(timeStr), "%02d:%02d", 
                 time->hour, time->minute);
    } else {
        // Odd seconds: show space at colon position (to achieve blinking effect)
        snprintf(timeStr, sizeof(timeStr), "%02d %02d", 
                 time->hour, time->minute);
    }
    
    // Format date string
    char dateStr[16];
    snprintf(dateStr, sizeof(dateStr), "%04d-%02d-%02d", 
             2000 + ds3231_time.year, ds3231_time.month, ds3231_time.date);
    
    // Format weekday string (DS3231 day: 1=Sunday, 2=Monday, ..., 7=Saturday)
    const char* weekdays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    const char* weekdayStr = (ds3231_time.day >= 1 && ds3231_time.day <= 7) ? 
                             weekdays[ds3231_time.day - 1] : "---";
    
    // Read temperature
    float temperature = 0.0f;
    char tempStr[12] = "---c";
    if (ds3231_read_temperature(&ds3231, &temperature)) {
        snprintf(tempStr, sizeof(tempStr), "%.1fc", temperature);
    }
    
    // Display complete clock interface (with pixel shift)
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

// NVS: Save last sync timestamp
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

// NVS: Read last sync timestamp
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

// Check if NTP sync is needed (returns false if synced within 720 hours)
static bool should_sync_ntp(void)
{
    // If force sync flag is set, return true directly (ignore 720-hour limit)
    // Use static variable to avoid duplicate log output
    static bool force_sync_logged = false;
    if (s_force_ntp_sync) {
        if (!force_sync_logged) {
            ESP_LOGI(TAG, "Force NTP sync requested");
            force_sync_logged = true;
        }
        return true;
    } else {
        // If force sync flag is cleared, reset log flag
        force_sync_logged = false;
    }
    
    time_t last_sync = get_last_sync_time();
    if (last_sync == 0) {
        ESP_LOGI(TAG, "No previous sync record found, will sync NTP");
        return true;
    }
    
    // Try to use system time, if unavailable use DS3231 time
    time_t now = time(NULL);
    if (now <= 0) {
        // System time unavailable, try to get time from DS3231
        ds3231_time_t ds3231_time;
        if (ds3231_read_time(&ds3231, &ds3231_time)) {
            // Convert DS3231 time to time_t (simplified calculation, only for time difference judgment)
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
    
    // Calculate time difference (seconds)
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

// WiFi connection status callback
static void wifi_status_callback(bool connected, const char* ip)
{
    if (connected) {
        // Note: wifi_provisioning.c has already output "Got IP" log
        // Only output connection success log once here to avoid duplication
        ESP_LOGI(TAG, "WiFi connected successfully! IP: %s", ip ? ip : "unknown");
        s_retry_num = 0;
        
        // If previously in provisioning mode, now connection successful, stop provisioning mode
        if (s_in_provisioning_mode) {
            ESP_LOGI(TAG, "Stopping provisioning mode");
            wifi_provisioning_stop_softap();
            s_in_provisioning_mode = false;
        }
    } else {
        ESP_LOGI(TAG, "WiFi disconnected");
    }
}

// WiFi event handler (for retry logic and status callback)
// Note: WIFI_EVENT_STA_START event has been handled in wifi_provisioning.c, not repeated here
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                              int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t* event = (wifi_event_sta_disconnected_t*) event_data;
        // Simplify log output to avoid stack overflow
        ESP_LOGW(TAG, "WiFi disconnected, reason: %d", event->reason);
        
        // If "No AP found" error, mark need for scan (execute in main loop to avoid stack overflow)
        if (event->reason == WIFI_REASON_NO_AP_FOUND && s_retry_num == 0) {
            s_need_wifi_scan = true;
        }
        
        if (s_retry_num < WIFI_MAX_RETRY) {
            // Note: Using vTaskDelay in event handler will block event loop
            // But this is necessary because we need delayed retry
            // Wait 15 seconds before retry connection
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
            // Mark need to enter provisioning mode (handle in main loop to avoid complex operations in event handler)
            s_need_enter_provisioning = true;
            wifi_status_callback(false, NULL);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        // Note: wifi_provisioning.c has already handled IP_EVENT_STA_GOT_IP and called callback
        // Not repeated here to avoid duplicate logs
        // Callback in wifi_provisioning.c is sufficient
    }
}

// Perform WiFi scan (independent function to avoid stack overflow)
static void perform_wifi_scan(void)
{
    ESP_LOGI(TAG, "Scanning for available WiFi networks...");
    wifi_scan_config_t scan_config = {
        .ssid = NULL,  // Scan all networks
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,  // Include hidden SSID
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = {
            .active = {
                .min = 100,
                .max = 300,
            }
        }
    };
    
    esp_err_t ret = esp_wifi_scan_start(&scan_config, true);  // Blocking scan
    if (ret == ESP_OK) {
        uint16_t ap_count = 0;
        esp_wifi_scan_get_ap_num(&ap_count);
        ESP_LOGI(TAG, "Found %d WiFi networks:", ap_count);
        
        if (ap_count > 0) {
            // Reduce array size to avoid stack overflow (display at most 10)
            wifi_ap_record_t ap_records[10];
            uint16_t ap_records_count = ap_count > 10 ? 10 : ap_count;
            esp_wifi_scan_get_ap_records(&ap_records_count, ap_records);
            
            // Read configured SSID
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

// Initialize WiFi Station mode
static esp_err_t wifi_init_sta(void)
{
    // Read WiFi configuration from NVS
    wifi_config_data_t wifi_config_data;
    esp_err_t ret = wifi_provisioning_load_config(&wifi_config_data);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load WiFi config from NVS: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "Initializing WiFi Station mode...");
    ESP_LOGI(TAG, "Connecting to SSID: %s", wifi_config_data.ssid);
    
    // Start WiFi Station
    ret = wifi_provisioning_start_sta(&wifi_config_data, wifi_status_callback);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WiFi Station: %s", esp_err_to_name(ret));
        return ret;
    }
    
    return ESP_OK;
}

// Close WiFi module to save power (deprecated, use wifi_provisioning_stop_softap instead)
static void wifi_deinit_sta(void)
{
    ESP_LOGI(TAG, "Deinitializing WiFi to save power...");
    wifi_provisioning_stop_softap();
}

// Check and sync NTP time to DS3231
static void sync_ntp_to_ds3231(void)
{
    if (sntp_synced) {
        return;  // Already synced
    }
    
    // Check if SNTP is synchronized
    time_t now = 0;
    struct tm timeinfo = {0};
    time(&now);
    
    if (now > 0) {
        localtime_r(&now, &timeinfo);
        
        // Check if time is reasonable (year should be between 2020-2099)
        if (timeinfo.tm_year >= 120 && timeinfo.tm_year < 200) {
            ESP_LOGI(TAG, "SNTP time synchronized!");
            sntp_synced = true;
            
            // If force sync, clear flag
            if (s_force_ntp_sync) {
                ESP_LOGI(TAG, "Force NTP sync completed successfully");
                s_force_ntp_sync = false;
                // Note: force_sync_logged will be automatically reset on next should_sync_ntp() call
            }
            
            // Sync NTP time to DS3231
            ds3231_time_t ds3231_time;
            ds3231_time.seconds = timeinfo.tm_sec;
            ds3231_time.minutes = timeinfo.tm_min;
            ds3231_time.hours = timeinfo.tm_hour;
            ds3231_time.day = timeinfo.tm_wday == 0 ? 7 : timeinfo.tm_wday;  // Convert to 1-7 (Sunday=7)
            ds3231_time.date = timeinfo.tm_mday;
            ds3231_time.month = timeinfo.tm_mon + 1;  // tm_mon is 0-11
            ds3231_time.year = timeinfo.tm_year - 100;  // tm_year is years since 1900, convert to 0-99
            
            if (ds3231_write_time(&ds3231, &ds3231_time)) {
                ESP_LOGI(TAG, "Time synchronized to DS3231: %04d-%02d-%02d %02d:%02d:%02d",
                         2000 + ds3231_time.year, ds3231_time.month, ds3231_time.date,
                         ds3231_time.hours, ds3231_time.minutes, ds3231_time.seconds);
                
                // Save sync timestamp to NVS
                save_last_sync_time(now);
                
                // Close WiFi after sync completes to save power
                wifi_deinit_sta();
                ntp_initialized = false;  // Mark no longer need to check NTP sync
            } else {
                ESP_LOGE(TAG, "Failed to write time to DS3231");
            }
        }
    }
}

// Initialize SNTP
static void sntp_init_func(void)
{
    ESP_LOGI(TAG, "Initializing SNTP...");
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    
    // Set NTP servers (Beijing time)
    sntp_setservername(0, NTP_SERVER1);
    sntp_setservername(1, NTP_SERVER2);
    sntp_setservername(2, NTP_SERVER3);
    
    // Set timezone (Beijing time UTC+8)
    // Note: Timezone has been set at the beginning of app_main(), set again here to ensure correctness
    setenv("TZ", "CST-8", 1);
    tzset();
    
    // Start SNTP
    sntp_init();
    
    ESP_LOGI(TAG, "SNTP initialized. Waiting for time sync...");
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== NTP Timer with DS3231 and SSD1306 ===");
    
    // Initialize NVS (for storing sync timestamp)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // NVS partition occupied or version mismatch, erase and reinitialize
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    // Set timezone (Beijing time UTC+8) - must be set before calling should_sync_ntp()
    // This way mktime() can correctly convert DS3231 local time to UTC timestamp
    setenv("TZ", "CST-8", 1);
    tzset();
    ESP_LOGI(TAG, "Timezone set to CST-8 (UTC+8)");
    
    // Initialize I2C bus (DS3231 and SSD1306 share)
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
    
    // Initialize DS3231
    ESP_LOGI(TAG, "Initializing DS3231 RTC...");
    if (!ds3231_init(&ds3231, i2c_bus, DS3231_SDA_PIN, DS3231_SCL_PIN)) {
        ESP_LOGE(TAG, "Failed to initialize DS3231!");
        ESP_LOGE(TAG, "Please check I2C connections (SDA=GPIO%d, SCL=GPIO%d)", DS3231_SDA_PIN, DS3231_SCL_PIN);
    } else {
        ESP_LOGI(TAG, "DS3231 initialized successfully");
        
        // Check oscillator status
        bool oscillator_stopped = false;
        if (ds3231_is_oscillator_stopped(&ds3231, &oscillator_stopped)) {
            if (oscillator_stopped) {
                ESP_LOGW(TAG, "Warning: DS3231 oscillator was stopped. Time may be inaccurate.");
            }
        }
    }
    
    // Initialize SSD1306 display module (shares I2C bus with DS3231)
    ESP_LOGI(TAG, "Initializing SSD1306 display...");
    // Try two common I2C addresses
    bool ssd1306_ok = false;
    if (ssd1306_init(&ssd1306, i2c_bus, SSD1306_I2C_ADDR_0)) {
        ESP_LOGI(TAG, "SSD1306 initialized successfully at address 0x%02X", SSD1306_I2C_ADDR_0);
        ssd1306_ok = true;
    } else {
        ESP_LOGW(TAG, "Failed to initialize SSD1306 at address 0x%02X, trying 0x%02X...", 
                 SSD1306_I2C_ADDR_0, SSD1306_I2C_ADDR_1);
        // If first address fails, try second address
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
    
    // Read time from DS3231 and display
    Time_t currentTime = {15, 29, 15};  // Default time
    if (readTimeFromDS3231(&currentTime)) {
        ESP_LOGI(TAG, "Time read from DS3231: %02d:%02d:%02d", 
               currentTime.hour, currentTime.minute, currentTime.second);
    } else {
        ESP_LOGW(TAG, "Failed to read time from DS3231, using default time: %02d:%02d:%02d",
                currentTime.hour, currentTime.minute, currentTime.second);
    }
    
    // Display initial time
    displayTime(&currentTime);
    
    // Initialize WiFi provisioning module (event handlers registered internally)
    ESP_LOGI(TAG, "Initializing WiFi provisioning module...");
    ESP_ERROR_CHECK(wifi_provisioning_init());
    
    // Register additional WiFi event handlers (for Station mode connection status and retry logic)
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
    
    // First check if there is saved WiFi configuration
    // If no configuration, directly enter provisioning mode
    ESP_LOGI(TAG, "Checking for saved WiFi config in NVS...");
    bool has_wifi_config = wifi_provisioning_has_config();
    
    if (!has_wifi_config) {
        // No WiFi config, automatically start SoftAP provisioning mode
        ESP_LOGI(TAG, "No WiFi config found in NVS. Automatically entering provisioning mode...");
        ESP_LOGI(TAG, "Please connect to WiFi hotspot 'PIX_Clock_Setup' and open http://192.168.4.1");
        s_in_provisioning_mode = true;
        ESP_ERROR_CHECK(wifi_provisioning_start_softap(wifi_status_callback));
    } else {
        // WiFi config exists, check if NTP sync is needed (determines if WiFi needs to be started)
        s_need_ntp_sync = should_sync_ntp();
        ESP_LOGI(TAG, "WiFi config found. NTP sync check: %s", s_need_ntp_sync ? "needed" : "not needed");
        
        if (s_need_ntp_sync) {
        // Config exists and NTP sync needed, start WiFi connection
        ESP_LOGI(TAG, "WiFi config found. NTP sync needed. Connecting to WiFi...");
        esp_err_t ret = wifi_init_sta();
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to start WiFi Station, entering provisioning mode");
                s_in_provisioning_mode = true;
                ESP_ERROR_CHECK(wifi_provisioning_start_softap(wifi_status_callback));
            } else {
                // WiFi started successfully, wait for connection then initialize SNTP
                ESP_LOGI(TAG, "NTP sync needed. Waiting for WiFi connection...");
                esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
                esp_netif_ip_info_t ip_info;
                int retry_count = 0;
                bool wifi_connected = false;
                
                while (retry_count < 30) {  // Wait at most 30 seconds
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
                    // WiFi connection failed, enter provisioning mode
                    wifi_provisioning_stop_softap();  // First stop Station mode
                    vTaskDelay(pdMS_TO_TICKS(500));  // Wait for WiFi to completely stop
                    s_retry_num = 0;  // Reset retry count
                    s_need_enter_provisioning = true;  // Mark need to enter provisioning mode
                    ntp_initialized = false;
                } else {
                    // WiFi connection successful, initialize SNTP
                    sntp_init_func();
                    ntp_initialized = true;
                }
            }
        } else {
            // Config exists but NTP sync not needed, don't start WiFi to save power
            ESP_LOGI(TAG, "WiFi config found but NTP sync not needed (last sync was within %d hours).", SYNC_INTERVAL_HOURS);
            ESP_LOGI(TAG, "Skipping WiFi initialization to save power. Using DS3231 time directly.");
            ntp_initialized = false;
            // Don't start WiFi, use DS3231 time directly
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
    TickType_t lastProvCheck = xTaskGetTickCount();  // Provisioning mode check
    TickType_t ntpSyncStartTime = xTaskGetTickCount();  // Record NTP sync start time
    const TickType_t updateIntervalMs = pdMS_TO_TICKS(1000);  // 1 second
    const TickType_t ntpCheckIntervalMs = pdMS_TO_TICKS(5000);  // Check NTP sync every 5 seconds
    const TickType_t provCheckIntervalMs = pdMS_TO_TICKS(2000);  // Check provisioning status every 2 seconds
    const TickType_t ntpSyncTimeoutMs = pdMS_TO_TICKS(60000);  // NTP sync timeout: 60 seconds
    
    while (1) {
        TickType_t now = xTaskGetTickCount();
        
        // If WiFi scan is needed (execute in main loop to avoid stack overflow in event handler)
        if (s_need_wifi_scan) {
            s_need_wifi_scan = false;
            // Move scan operation to independent function to avoid using large arrays in main loop
            perform_wifi_scan();
        }
        
        // If all 5 retries fail, enter provisioning mode
        if (s_need_enter_provisioning) {
            s_need_enter_provisioning = false;
            ESP_LOGI(TAG, "Entering provisioning mode due to connection failure...");
            
            // Stop current WiFi (whether Station or SoftAP mode)
            esp_err_t ret = esp_wifi_stop();
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Failed to stop WiFi: %s", esp_err_to_name(ret));
            }
            vTaskDelay(pdMS_TO_TICKS(500));  // Wait for WiFi to completely stop
            
            // Clear old invalid config to avoid detecting old config immediately after provisioning mode starts
            ESP_LOGI(TAG, "Clearing invalid WiFi config...");
            wifi_provisioning_clear_config();
            
            // Reset retry count
            s_retry_num = 0;
            
            // Start SoftAP provisioning mode
            s_in_provisioning_mode = true;
            ret = wifi_provisioning_start_softap(wifi_status_callback);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to start provisioning mode: %s", esp_err_to_name(ret));
            } else {
                ESP_LOGI(TAG, "Provisioning mode started. Connect to 'PIX_Clock_Setup' and visit http://192.168.4.1");
            }
        }
        
        // In provisioning mode, periodically check if new config is saved
        if (s_in_provisioning_mode && (now - lastProvCheck) >= provCheckIntervalMs) {
            lastProvCheck = now;
            if (wifi_provisioning_has_config()) {
                // New config detected, stop provisioning mode and connect WiFi
                ESP_LOGI(TAG, "WiFi config detected, stopping provisioning and connecting...");
                wifi_provisioning_stop_softap();
                s_in_provisioning_mode = false;
                
                // Start Station mode
                esp_err_t ret = wifi_init_sta();
                if (ret == ESP_OK) {
                    // Note: Don't initialize NTP here because WiFi may not have connected successfully yet
                    // Main loop will detect WiFi connection and initialize SNTP (see logic at lines 710-722)
                }
            }
        }
        
        // Periodically check NTP sync status (every 5 seconds, only when NTP is initialized)
        if (ntp_initialized && (now - lastNtpCheck) >= ntpCheckIntervalMs) {
            sync_ntp_to_ds3231();
            lastNtpCheck = now;
            
            // Check if timeout (not synced successfully within 60 seconds)
            if (!sntp_synced && (now - ntpSyncStartTime) >= ntpSyncTimeoutMs) {
                ESP_LOGW(TAG, "NTP sync timeout after 60 seconds. Closing WiFi to save power.");
                wifi_provisioning_stop_softap();
                ntp_initialized = false;
            }
        }
        
        // If WiFi is connected and NTP sync is needed but NTP is not initialized, initialize NTP
        // Note: If s_force_ntp_sync is true, need to recheck should_sync_ntp() to update s_need_ntp_sync
        // But only check when WiFi is connected to avoid repeated calls to should_sync_ntp()
        if (s_force_ntp_sync && !ntp_initialized) {
            esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            esp_netif_ip_info_t ip_info;
            // Only update s_need_ntp_sync when WiFi is connected to avoid duplicate logs
            if (sta_netif && esp_netif_get_ip_info(sta_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
                s_need_ntp_sync = should_sync_ntp();  // Recheck, should_sync_ntp will return true at this time (only output log once)
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
            // Read latest time from DS3231
            if (!readTimeFromDS3231(&currentTime)) {
                // If read fails, use software timing (backward compatibility)
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
