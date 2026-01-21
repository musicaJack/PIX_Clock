#ifndef WIFI_PROVISIONING_H
#define WIFI_PROVISIONING_H

#include "esp_err.h"
#include "esp_wifi.h"

#ifdef __cplusplus
extern "C" {
#endif

// WiFi 配置结构
typedef struct {
    char ssid[33];      // WiFi SSID（最大32字符 + null terminator）
    char password[65];  // WiFi 密码（最大64字符 + null terminator）
} wifi_config_data_t;

// 配网状态回调函数类型
typedef void (*wifi_prov_status_cb_t)(bool connected, const char* ip);

/**
 * @brief 初始化 WiFi 配网模块
 * 
 * @return 
 *    - ESP_OK: 成功
 *    - 其他: 失败
 */
esp_err_t wifi_provisioning_init(void);

/**
 * @brief 从 NVS 读取 WiFi 配置
 * 
 * @param config 输出参数，保存读取的配置
 * @return 
 *    - ESP_OK: 成功读取配置
 *    - ESP_ERR_NOT_FOUND: 未找到配置
 *    - 其他: 读取失败
 */
esp_err_t wifi_provisioning_load_config(wifi_config_data_t *config);

/**
 * @brief 保存 WiFi 配置到 NVS
 * 
 * @param config 要保存的配置
 * @return 
 *    - ESP_OK: 成功
 *    - 其他: 失败
 */
esp_err_t wifi_provisioning_save_config(const wifi_config_data_t *config);

/**
 * @brief 启动 SoftAP 配网模式
 * 
 * @param status_cb 配网状态回调函数（可选，可为 NULL）
 * @return 
 *    - ESP_OK: 成功
 *    - 其他: 失败
 */
esp_err_t wifi_provisioning_start_softap(wifi_prov_status_cb_t status_cb);

/**
 * @brief 停止 SoftAP 配网模式
 * 
 * @return 
 *    - ESP_OK: 成功
 *    - 其他: 失败
 */
esp_err_t wifi_provisioning_stop_softap(void);

/**
 * @brief 启动 WiFi Station 模式并连接
 * 
 * @param config WiFi 配置
 * @param status_cb 连接状态回调函数（可选，可为 NULL）
 * @return 
 *    - ESP_OK: 成功启动连接
 *    - 其他: 失败
 */
esp_err_t wifi_provisioning_start_sta(const wifi_config_data_t *config, wifi_prov_status_cb_t status_cb);

/**
 * @brief 检查是否有保存的 WiFi 配置
 * 
 * @return true 有配置，false 无配置
 */
bool wifi_provisioning_has_config(void);

/**
 * @brief 清除保存的 WiFi 配置
 * 
 * @return 
 *    - ESP_OK: 成功
 *    - 其他: 失败
 */
esp_err_t wifi_provisioning_clear_config(void);

#ifdef __cplusplus
}
#endif

#endif // WIFI_PROVISIONING_H
