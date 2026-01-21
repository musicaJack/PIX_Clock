idf_component_register(SRCS "main.c"
                            "lib/ds3231/ds3231_driver.c"
                            "lib/ssd1306/ssd1306.c"
                            "lib/wifi_provisioning/wifi_provisioning.c"
                    INCLUDE_DIRS "." "lib/ds3231" "lib/ssd1306" "lib/wifi_provisioning"
                    PRIV_REQUIRES driver esp_wifi esp_netif lwip nvs_flash esp_http_server esp_timer)
