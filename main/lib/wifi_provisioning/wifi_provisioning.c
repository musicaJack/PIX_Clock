#include "wifi_provisioning.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>
#include <sys/param.h>
#include <ctype.h>

static const char *TAG = "wifi_prov";

// NVS 配置
// 注意：使用独立的命名空间 "wifi_config"，与项目中其他 NVS 命名空间（如 "time_sync"）隔离
// 因此不会覆盖或影响其他模块的 NVS 数据
#define NVS_NAMESPACE_WIFI     "wifi_config"
#define NVS_KEY_SSID           "ssid"
#define NVS_KEY_PASSWORD       "password"

// SoftAP 配置
#define SOFTAP_SSID            "VFD_Clock_Setup"
#define SOFTAP_PASSWORD        "12345678"
#define SOFTAP_CHANNEL         1
#define SOFTAP_MAX_CONNECTIONS 4

// 全局变量
static httpd_handle_t s_httpd_handle = NULL;
static wifi_prov_status_cb_t s_status_cb = NULL;
static esp_netif_t *s_ap_netif = NULL;
static esp_netif_t *s_sta_netif = NULL;
static bool s_wifi_connected = false;
static char s_connected_ip[16] = {0};

// 配网网页 HTML
static const char* PROVISIONING_HTML = 
"<!DOCTYPE html>"
"<html>"
"<head>"
"<meta charset='UTF-8'>"
"<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
"<title>WiFi 配网</title>"
"<style>"
"body { font-family: Arial, sans-serif; margin: 20px; background: #f5f5f5; }"
".container { max-width: 400px; margin: 50px auto; background: white; padding: 30px; border-radius: 10px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }"
"h1 { color: #333; text-align: center; margin-bottom: 30px; }"
"label { display: block; margin: 15px 0 5px; color: #555; font-weight: bold; }"
"input { width: 100%; padding: 10px; border: 1px solid #ddd; border-radius: 5px; box-sizing: border-box; font-size: 14px; }"
"button { width: 100%; padding: 12px; background: #007bff; color: white; border: none; border-radius: 5px; font-size: 16px; cursor: pointer; margin-top: 20px; }"
"button:hover { background: #0056b3; }"
"button:disabled { background: #ccc; cursor: not-allowed; }"
".status { margin-top: 20px; padding: 10px; border-radius: 5px; text-align: center; }"
".success { background: #d4edda; color: #155724; }"
".error { background: #f8d7da; color: #721c24; }"
"</style>"
"</head>"
"<body>"
"<div class='container'>"
"<h1>WiFi 配网</h1>"
"<form id='wifiForm'>"
"<label for='ssid'>WiFi 名称 (SSID):</label>"
"<input type='text' id='ssid' name='ssid' required autocomplete='off'>"
"<label for='password'>WiFi 密码:</label>"
"<input type='password' id='password' name='password' autocomplete='off'>"
"<button type='submit'>连接</button>"
"</form>"
"<div id='status'></div>"
"</div>"
"<script>"
"document.getElementById('wifiForm').addEventListener('submit', async function(e) {"
"e.preventDefault();"
"const ssid = document.getElementById('ssid').value;"
"const password = document.getElementById('password').value;"
"const statusDiv = document.getElementById('status');"
"const button = document.querySelector('button');"
"button.disabled = true;"
"button.textContent = '连接中...';"
"statusDiv.innerHTML = '';"
"try {"
"const formData = new URLSearchParams();"
"formData.append('ssid', ssid);"
"formData.append('password', password);"
"const response = await fetch('/wifi', {"
"method: 'POST',"
"headers: { 'Content-Type': 'application/x-www-form-urlencoded' },"
"body: formData"
"});"
"const text = await response.text();"
"let data;"
"try { data = JSON.parse(text); } catch(e) { data = {success: false, message: text}; }"
"if (data.success) {"
"statusDiv.className = 'status success';"
"statusDiv.innerHTML = '配置成功！设备正在连接 WiFi，请稍候...';"
"setTimeout(() => { statusDiv.innerHTML += '<br>如果连接成功，设备将断开此热点。'; }, 2000);"
"} else {"
"statusDiv.className = 'status error';"
"statusDiv.innerHTML = '配置失败: ' + (data.message || '未知错误');"
"button.disabled = false;"
"button.textContent = '连接';"
"}"
"} catch (error) {"
"statusDiv.className = 'status error';"
"statusDiv.innerHTML = '网络错误: ' + error.message;"
"button.disabled = false;"
"button.textContent = '连接';"
"}"
"});"
"</script>"
"</body>"
"</html>";

// HTTP 处理函数：根路径
static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, PROVISIONING_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// URL 解码函数
static void url_decode(char *str) {
    char *src = str;
    char *dst = str;
    
    while (*src) {
        // 将 char 转换为 unsigned char 以符合 isxdigit 的要求
        if (*src == '%' && isxdigit((unsigned char)src[1]) && isxdigit((unsigned char)src[2])) {
            int value;
            sscanf(src + 1, "%2x", &value);
            *dst++ = (char)value;
            src += 3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

// 从 URL 编码的表单数据中提取值
static char* get_form_value(const char *data, const char *key, char *value, size_t value_len) {
    char key_pattern[64];
    snprintf(key_pattern, sizeof(key_pattern), "%s=", key);
    
    const char *start = strstr(data, key_pattern);
    if (!start) {
        return NULL;
    }
    
    start += strlen(key_pattern);
    const char *end = strchr(start, '&');
    if (!end) {
        end = start + strlen(start);
    }
    
    size_t len = MIN((size_t)(end - start), value_len - 1);
    strncpy(value, start, len);
    value[len] = '\0';
    url_decode(value);
    
    return value;
}

// HTTP 处理函数：WiFi 配置 POST
static esp_err_t wifi_post_handler(httpd_req_t *req)
{
    char content[256];
    
    // 读取请求内容
    int recv_len = httpd_req_recv(req, content, sizeof(content) - 1);
    if (recv_len <= 0) {
        httpd_resp_set_status(req, HTTPD_400);
        httpd_resp_send(req, "Bad Request", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }
    content[recv_len] = '\0';
    
    ESP_LOGI(TAG, "Received WiFi config: %s", content);
    
    // 解析表单数据
    wifi_config_data_t config;
    config.ssid[0] = '\0';
    config.password[0] = '\0';
    
    if (!get_form_value(content, "ssid", config.ssid, sizeof(config.ssid))) {
        ESP_LOGE(TAG, "SSID not found in form data");
        httpd_resp_set_status(req, HTTPD_400);
        httpd_resp_send(req, "{\"success\":false,\"message\":\"SSID required\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }
    
    get_form_value(content, "password", config.password, sizeof(config.password));
    
    ESP_LOGI(TAG, "Saving WiFi config: SSID=%s, Password=%s", config.ssid, 
             strlen(config.password) > 0 ? "***" : "(empty)");
    
    // 保存配置
    esp_err_t err = wifi_provisioning_save_config(&config);
    
    // 发送响应
    httpd_resp_set_type(req, "application/json");
    if (err == ESP_OK) {
        httpd_resp_send(req, "{\"success\":true,\"message\":\"Config saved\"}", HTTPD_RESP_USE_STRLEN);
        ESP_LOGI(TAG, "WiFi config saved successfully");
    } else {
        httpd_resp_set_status(req, HTTPD_500);
        httpd_resp_send(req, "{\"success\":false,\"message\":\"Failed to save config\"}", HTTPD_RESP_USE_STRLEN);
        ESP_LOGE(TAG, "Failed to save WiFi config: %s", esp_err_to_name(err));
    }
    
    return ESP_OK;
}

// WiFi 事件处理（配网模块内部）
static void wifi_prov_event_handler(void* arg, esp_event_base_t event_base,
                              int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_AP_START:
                ESP_LOGI(TAG, "SoftAP started");
                break;
            case WIFI_EVENT_AP_STOP:
                ESP_LOGI(TAG, "SoftAP stopped");
                break;
            case WIFI_EVENT_AP_STACONNECTED:
                {
                    wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
                    ESP_LOGI(TAG, "Station joined, AID=%d, MAC=" MACSTR,
                            event->aid, MAC2STR(event->mac));
                }
                break;
            case WIFI_EVENT_AP_STADISCONNECTED:
                {
                    wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
                    ESP_LOGI(TAG, "Station left, AID=%d, MAC=" MACSTR,
                            event->aid, MAC2STR(event->mac));
                }
                break;
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "WiFi Station started");
                // 延迟一小段时间确保 WiFi 驱动完全初始化
                vTaskDelay(pdMS_TO_TICKS(100));
                esp_err_t ret = esp_wifi_connect();
                if (ret != ESP_OK) {
                    ESP_LOGW(TAG, "esp_wifi_connect() failed: %s", esp_err_to_name(ret));
                }
                break;
            case WIFI_EVENT_STA_DISCONNECTED:
                {
                    wifi_event_sta_disconnected_t* event = (wifi_event_sta_disconnected_t*) event_data;
                    ESP_LOGI(TAG, "WiFi Station disconnected, reason: %d", event->reason);
                    s_wifi_connected = false;
                    if (s_status_cb) {
                        s_status_cb(false, NULL);
                    }
                }
                break;
            default:
                break;
        }
    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_AP_STAIPASSIGNED) {
            ip_event_ap_staipassigned_t* event = (ip_event_ap_staipassigned_t*) event_data;
            ESP_LOGI(TAG, "Station IP assigned: " IPSTR, IP2STR(&event->ip));
        } else if (event_id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
            ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
            snprintf(s_connected_ip, sizeof(s_connected_ip), IPSTR, IP2STR(&event->ip_info.ip));
            s_wifi_connected = true;
            if (s_status_cb) {
                s_status_cb(true, s_connected_ip);
            }
        }
    }
}

// 初始化 HTTP 服务器
static esp_err_t start_http_server(void)
{
    if (s_httpd_handle != NULL) {
        return ESP_OK;  // 已经启动
    }
    
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 2;
    
    ESP_LOGI(TAG, "Starting HTTP server on port: '%d'", config.server_port);
    if (httpd_start(&s_httpd_handle, &config) == ESP_OK) {
        // 注册 URI 处理器
        httpd_uri_t root = {
            .uri       = "/",
            .method    = HTTP_GET,
            .handler   = root_get_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(s_httpd_handle, &root);
        
        httpd_uri_t wifi = {
            .uri       = "/wifi",
            .method    = HTTP_POST,
            .handler   = wifi_post_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(s_httpd_handle, &wifi);
        
        ESP_LOGI(TAG, "HTTP server started");
        return ESP_OK;
    }
    
    ESP_LOGE(TAG, "Failed to start HTTP server");
    return ESP_FAIL;
}

// 停止 HTTP 服务器
static void stop_http_server(void)
{
    if (s_httpd_handle != NULL) {
        httpd_stop(s_httpd_handle);
        s_httpd_handle = NULL;
        ESP_LOGI(TAG, "HTTP server stopped");
    }
}

esp_err_t wifi_provisioning_init(void)
{
    ESP_LOGI(TAG, "Initializing WiFi provisioning module");
    
    // 初始化网络接口
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    // 创建 SoftAP 和 Station 网络接口
    s_ap_netif = esp_netif_create_default_wifi_ap();
    s_sta_netif = esp_netif_create_default_wifi_sta();
    
    // 初始化 WiFi
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    // 注册事件处理器
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_prov_event_handler,
                                                        NULL,
                                                        NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_prov_event_handler,
                                                        NULL,
                                                        NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_AP_STAIPASSIGNED,
                                                        &wifi_prov_event_handler,
                                                        NULL,
                                                        NULL));
    
    return ESP_OK;
}

esp_err_t wifi_provisioning_load_config(wifi_config_data_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE_WIFI, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return err;
    }
    
    // 读取 SSID
    size_t required_size = sizeof(config->ssid);
    err = nvs_get_str(nvs_handle, NVS_KEY_SSID, config->ssid, &required_size);
    if (err != ESP_OK) {
        nvs_close(nvs_handle);
        ESP_LOGD(TAG, "SSID not found in NVS: %s", esp_err_to_name(err));
        return err;
    }
    
    // 读取密码
    required_size = sizeof(config->password);
    err = nvs_get_str(nvs_handle, NVS_KEY_PASSWORD, config->password, &required_size);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(nvs_handle);
        ESP_LOGE(TAG, "Failed to read password: %s", esp_err_to_name(err));
        return err;
    }
    
    // 如果密码不存在，设置为空字符串
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        config->password[0] = '\0';
    }
    
    nvs_close(nvs_handle);
    
    ESP_LOGI(TAG, "WiFi config loaded: SSID=%s, Password=%s", 
             config->ssid, strlen(config->password) > 0 ? "***" : "(empty)");
    
    return ESP_OK;
}

esp_err_t wifi_provisioning_save_config(const wifi_config_data_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE_WIFI, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return err;
    }
    
    // 保存 SSID
    err = nvs_set_str(nvs_handle, NVS_KEY_SSID, config->ssid);
    if (err != ESP_OK) {
        nvs_close(nvs_handle);
        ESP_LOGE(TAG, "Failed to save SSID: %s", esp_err_to_name(err));
        return err;
    }
    
    // 保存密码
    err = nvs_set_str(nvs_handle, NVS_KEY_PASSWORD, config->password);
    if (err != ESP_OK) {
        nvs_close(nvs_handle);
        ESP_LOGE(TAG, "Failed to save password: %s", esp_err_to_name(err));
        return err;
    }
    
    err = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(err));
        return err;
    }
    
    ESP_LOGI(TAG, "WiFi config saved successfully");
    return ESP_OK;
}

esp_err_t wifi_provisioning_start_softap(wifi_prov_status_cb_t status_cb)
{
    s_status_cb = status_cb;
    
    ESP_LOGI(TAG, "Starting SoftAP: SSID=%s, Password=%s", SOFTAP_SSID, SOFTAP_PASSWORD);
    
    // 配置 SoftAP
    wifi_config_t wifi_config = {
        .ap = {
            .ssid = SOFTAP_SSID,
            .ssid_len = strlen(SOFTAP_SSID),
            .password = SOFTAP_PASSWORD,
            .channel = SOFTAP_CHANNEL,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .max_connection = SOFTAP_MAX_CONNECTIONS,
        },
    };
    
    if (strlen(SOFTAP_PASSWORD) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    // 启动 HTTP 服务器
    esp_err_t ret = start_http_server();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return ret;
    }
    
    ESP_LOGI(TAG, "SoftAP started. Connect to '%s' with password '%s'", 
             SOFTAP_SSID, SOFTAP_PASSWORD);
    ESP_LOGI(TAG, "Then open http://192.168.4.1 in your browser");
    
    return ESP_OK;
}

esp_err_t wifi_provisioning_stop_softap(void)
{
    ESP_LOGI(TAG, "Stopping SoftAP");
    
    // 停止 HTTP 服务器
    stop_http_server();
    
    // 停止 WiFi
    esp_err_t ret = esp_wifi_stop();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to stop WiFi: %s", esp_err_to_name(ret));
    }
    
    return ESP_OK;
}

esp_err_t wifi_provisioning_start_sta(const wifi_config_data_t *config, wifi_prov_status_cb_t status_cb)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    
    s_status_cb = status_cb;
    s_wifi_connected = false;
    
    ESP_LOGI(TAG, "Starting WiFi Station: SSID=%s", config->ssid);
    
    // 配置 Station
    // 注意：threshold.authmode 设置为 WIFI_AUTH_OPEN 以支持所有认证模式
    // 这样可以自动适配 WPA、WPA2、WPA3 等不同的认证方式
    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_OPEN,  // 支持所有认证模式
            .scan_method = WIFI_FAST_SCAN,  // 快速扫描
            .sort_method = WIFI_CONNECT_AP_BY_SIGNAL,  // 按信号强度排序
        },
    };
    
    strncpy((char*)wifi_config.sta.ssid, config->ssid, sizeof(wifi_config.sta.ssid) - 1);
    wifi_config.sta.ssid[sizeof(wifi_config.sta.ssid) - 1] = '\0';
    
    if (strlen(config->password) > 0) {
        strncpy((char*)wifi_config.sta.password, config->password, sizeof(wifi_config.sta.password) - 1);
        wifi_config.sta.password[sizeof(wifi_config.sta.password) - 1] = '\0';
    }
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    return ESP_OK;
}

bool wifi_provisioning_has_config(void)
{
    wifi_config_data_t config;
    esp_err_t ret = wifi_provisioning_load_config(&config);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "WiFi config found: SSID=%s", config.ssid);
        return true;
    } else {
        ESP_LOGI(TAG, "No WiFi config found: %s", esp_err_to_name(ret));
        return false;
    }
}

esp_err_t wifi_provisioning_clear_config(void)
{
    // 注意：nvs_erase_all() 只会擦除当前命名空间 "wifi_config" 的数据
    // 不会影响其他命名空间（如 "time_sync"）的数据
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE_WIFI, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }
    
    err = nvs_erase_all(nvs_handle);
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }
    
    nvs_close(nvs_handle);
    return err;
}
