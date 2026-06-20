idf_component_register(
    SRCS "dashboard.cpp" "notes_board.cpp" "weather.cpp"
    INCLUDE_DIRS "include"
    REQUIRES json esp_http_client esp-tls system_services env_sensors zlib
)
