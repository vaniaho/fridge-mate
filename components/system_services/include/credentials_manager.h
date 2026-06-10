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

#ifdef __cplusplus
}
#endif
