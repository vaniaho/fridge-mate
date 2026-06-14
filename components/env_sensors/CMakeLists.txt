idf_component_register(
    SRCS "rtc_time.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_netif esp_event
)
