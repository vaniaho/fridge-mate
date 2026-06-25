idf_component_register(
    SRCS "wifi_manager.c" "sd_storage.c" "credentials_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash esp_wifi esp_wifi_remote esp_hosted esp_event esp_netif fatfs driver esp_hw_support json
)
