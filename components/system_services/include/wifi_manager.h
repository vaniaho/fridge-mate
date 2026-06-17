#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Connect to WiFi AP (blocking)
 * Uses ESP-Hosted via ESP32-C6 co-processor
 * @param ssid WiFi SSID
 * @param pass WiFi password
 */
void wifi_init_sta(const char* ssid, const char* pass);
void wifi_manager_disable_retry(void);

#ifdef __cplusplus
}
#endif
