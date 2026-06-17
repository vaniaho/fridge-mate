idf_component_register(SRCS "system_manager.cpp"
                    INCLUDE_DIRS "include"
                    REQUIRES nvs_flash esp_wifi esp_netif lwip esp_event esp_hw_support waveshare__esp32_p4_wifi6_touch_lcd_7b system_services)
