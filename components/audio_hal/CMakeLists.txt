idf_component_register(
    SRCS "realtime_client.cpp" "tts_client.cpp" "volc_binary_protocol.cpp" "websocket_diagnostics.cpp" "audio_hal.cpp" "asr_client.cpp"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_http_client
        esp-tls
        json
        system_services
        main
        freertos
        esp_ringbuf
        esp_codec_dev
        zlib
        esp_websocket_client
        waveshare__esp32_p4_wifi6_touch_lcd_7b
)
