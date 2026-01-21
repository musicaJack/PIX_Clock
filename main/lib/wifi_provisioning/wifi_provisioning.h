#ifndef WIFI_PROVISIONING_H
#define WIFI_PROVISIONING_H

#include "esp_err.h"
#include "esp_wifi.h"

#ifdef __cplusplus
extern "C" {
#endif

// WiFi configuration structure
typedef struct {
    char ssid[33];      // WiFi SSID (max 32 characters + null terminator)
    char password[65];  // WiFi password (max 64 characters + null terminator)
} wifi_config_data_t;

// Provisioning status callback function type
typedef void (*wifi_prov_status_cb_t)(bool connected, const char* ip);

/**
 * @brief Initialize WiFi provisioning module
 * 
 * @return 
 *    - ESP_OK: Success
 *    - Others: Failure
 */
esp_err_t wifi_provisioning_init(void);

/**
 * @brief Load WiFi configuration from NVS
 * 
 * @param config Output parameter, stores the loaded configuration
 * @return 
 *    - ESP_OK: Successfully loaded configuration
 *    - ESP_ERR_NOT_FOUND: Configuration not found
 *    - Others: Failed to load
 */
esp_err_t wifi_provisioning_load_config(wifi_config_data_t *config);

/**
 * @brief Save WiFi configuration to NVS
 * 
 * @param config Configuration to save
 * @return 
 *    - ESP_OK: Success
 *    - Others: Failure
 */
esp_err_t wifi_provisioning_save_config(const wifi_config_data_t *config);

/**
 * @brief Start SoftAP provisioning mode
 * 
 * @param status_cb Provisioning status callback function (optional, can be NULL)
 * @return 
 *    - ESP_OK: Success
 *    - Others: Failure
 */
esp_err_t wifi_provisioning_start_softap(wifi_prov_status_cb_t status_cb);

/**
 * @brief Stop SoftAP provisioning mode
 * 
 * @return 
 *    - ESP_OK: Success
 *    - Others: Failure
 */
esp_err_t wifi_provisioning_stop_softap(void);

/**
 * @brief Start WiFi Station mode and connect
 * 
 * @param config WiFi configuration
 * @param status_cb Connection status callback function (optional, can be NULL)
 * @return 
 *    - ESP_OK: Successfully started connection
 *    - Others: Failure
 */
esp_err_t wifi_provisioning_start_sta(const wifi_config_data_t *config, wifi_prov_status_cb_t status_cb);

/**
 * @brief Check if there is saved WiFi configuration
 * 
 * @return true if configuration exists, false otherwise
 */
bool wifi_provisioning_has_config(void);

/**
 * @brief Clear saved WiFi configuration
 * 
 * @return 
 *    - ESP_OK: Success
 *    - Others: Failure
 */
esp_err_t wifi_provisioning_clear_config(void);

#ifdef __cplusplus
}
#endif

#endif // WIFI_PROVISIONING_H
