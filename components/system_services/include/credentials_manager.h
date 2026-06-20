#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the credentials manager
 * Reads saved credentials from NVS. If no saved values exist,
 * falls back to Kconfig default values.
 * Must be called after nvs_flash_init().
 */
esp_err_t credentials_init(void);

// ======== WiFi Credentials ========
const char* credentials_get_wifi_ssid(void);
const char* credentials_get_wifi_pass(void);
esp_err_t credentials_set_wifi(const char* ssid, const char* pass);

// ======== LLM API Credentials ========
const char* credentials_get_llm_api_url(void);
const char* credentials_get_llm_api_key(void);
const char* credentials_get_llm_model(void);
esp_err_t credentials_set_llm_api(const char* url, const char* key);
esp_err_t credentials_set_llm_model(const char* model);

// ======== Weather API Config ========
// 支持用户在 Web 设置页配置天气服务商（和风/心知等）。
// wx_url 含 {location} 和 {key} 占位符，运行时替换；
// wx_temp_path / wx_text_path 为 cJSON 字段路径（如 "now.temp"），适配不同服务商的 JSON 结构。
const char* credentials_get_weather_url(void);
const char* credentials_get_weather_key(void);
const char* credentials_get_weather_city(void);
const char* credentials_get_weather_location(void);
const char* credentials_get_weather_temp_path(void);
const char* credentials_get_weather_text_path(void);
esp_err_t credentials_set_weather_url(const char* url);
esp_err_t credentials_set_weather_key(const char* key);
esp_err_t credentials_set_weather_city(const char* city);
esp_err_t credentials_set_weather_location(const char* location);
esp_err_t credentials_set_weather_temp_path(const char* path);
esp_err_t credentials_set_weather_text_path(const char* path);

#ifdef __cplusplus
}
#endif
