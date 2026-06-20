#include "credentials_manager.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "CredsMgr";

#define NVS_NAMESPACE "creds"
#define BUF_SIZE 256

// Static buffers for credential storage
static char s_wifi_ssid[BUF_SIZE];
static char s_wifi_pass[BUF_SIZE];
static char s_llm_url[BUF_SIZE];
static char s_llm_key[BUF_SIZE];
static char s_llm_model[BUF_SIZE];
// Weather API config (URL 含占位符，可能较长，用更大缓冲)
static char s_wx_url[512];
static char s_wx_key[BUF_SIZE];
static char s_wx_city[BUF_SIZE];
static char s_wx_location[BUF_SIZE];
static char s_wx_temp_path[BUF_SIZE];
static char s_wx_text_path[BUF_SIZE];

/**
 * @brief Helper: read a string from NVS. If key not found, use fallback default.
 */
static esp_err_t load_nvs_string(nvs_handle_t handle, const char* key,
                                  char* buf, size_t buf_size,
                                  const char* default_val) {
    size_t required_size = buf_size;
    esp_err_t err = nvs_get_str(handle, key, buf, &required_size);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        // Key doesn't exist in NVS yet, use Kconfig default
        strncpy(buf, default_val, buf_size - 1);
        buf[buf_size - 1] = '\0';
        ESP_LOGI(TAG, "Key '%s' not in NVS, using default: '%s'", key,
                 strcmp(key, "llm_key") == 0 ? "***" : buf);
        return ESP_OK;
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read '%s' from NVS: %s", key, esp_err_to_name(err));
        strncpy(buf, default_val, buf_size - 1);
        buf[buf_size - 1] = '\0';
        return err;
    }
    ESP_LOGI(TAG, "Loaded '%s' from NVS", key);
    return ESP_OK;
}

/**
 * @brief Helper: write a string to NVS and commit.
 */
static esp_err_t save_nvs_string(const char* key, const char* value) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for write: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(handle, key, value);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set '%s': %s", key, esp_err_to_name(err));
        nvs_close(handle);
        return err;
    }

    err = nvs_commit(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(err));
    }

    nvs_close(handle);
    return err;
}

esp_err_t credentials_init(void) {
    ESP_LOGI(TAG, "Initializing credentials manager...");

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        // Namespace doesn't exist yet, that's fine — use all defaults
        ESP_LOGI(TAG, "NVS namespace '%s' not found, using all Kconfig defaults", NVS_NAMESPACE);
        strncpy(s_wifi_ssid, CONFIG_SMART_FRIDGE_DEFAULT_WIFI_SSID, BUF_SIZE - 1);
        strncpy(s_wifi_pass, CONFIG_SMART_FRIDGE_DEFAULT_WIFI_PASS, BUF_SIZE - 1);
        strncpy(s_llm_url, CONFIG_SMART_FRIDGE_DEFAULT_LLM_API_URL, BUF_SIZE - 1);
        strncpy(s_llm_key, CONFIG_SMART_FRIDGE_DEFAULT_LLM_API_KEY, BUF_SIZE - 1);
        strncpy(s_llm_model, CONFIG_SMART_FRIDGE_DEFAULT_LLM_MODEL, BUF_SIZE - 1);
        strncpy(s_wx_url, CONFIG_SMART_FRIDGE_DEFAULT_WEATHER_URL, sizeof(s_wx_url) - 1);
        strncpy(s_wx_key, CONFIG_SMART_FRIDGE_DEFAULT_WEATHER_KEY, BUF_SIZE - 1);
        strncpy(s_wx_city, CONFIG_SMART_FRIDGE_DEFAULT_WEATHER_CITY, BUF_SIZE - 1);
        strncpy(s_wx_location, CONFIG_SMART_FRIDGE_DEFAULT_WEATHER_LOCATION, BUF_SIZE - 1);
        strncpy(s_wx_temp_path, CONFIG_SMART_FRIDGE_DEFAULT_WEATHER_TEMP_PATH, BUF_SIZE - 1);
        strncpy(s_wx_text_path, CONFIG_SMART_FRIDGE_DEFAULT_WEATHER_TEXT_PATH, BUF_SIZE - 1);
        return ESP_OK;
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace '%s': %s", NVS_NAMESPACE, esp_err_to_name(err));
        return err;
    }

    // Load each credential from NVS, falling back to Kconfig defaults
    load_nvs_string(handle, "wifi_ssid", s_wifi_ssid, BUF_SIZE, CONFIG_SMART_FRIDGE_DEFAULT_WIFI_SSID);
    load_nvs_string(handle, "wifi_pass", s_wifi_pass, BUF_SIZE, CONFIG_SMART_FRIDGE_DEFAULT_WIFI_PASS);
    load_nvs_string(handle, "llm_url",   s_llm_url,   BUF_SIZE, CONFIG_SMART_FRIDGE_DEFAULT_LLM_API_URL);
    load_nvs_string(handle, "llm_key",   s_llm_key,   BUF_SIZE, CONFIG_SMART_FRIDGE_DEFAULT_LLM_API_KEY);
    load_nvs_string(handle, "llm_model", s_llm_model, BUF_SIZE, CONFIG_SMART_FRIDGE_DEFAULT_LLM_MODEL);
    load_nvs_string(handle, "wx_url",       s_wx_url,       sizeof(s_wx_url), CONFIG_SMART_FRIDGE_DEFAULT_WEATHER_URL);
    load_nvs_string(handle, "wx_key",       s_wx_key,       BUF_SIZE, CONFIG_SMART_FRIDGE_DEFAULT_WEATHER_KEY);
    load_nvs_string(handle, "wx_city",      s_wx_city,      BUF_SIZE, CONFIG_SMART_FRIDGE_DEFAULT_WEATHER_CITY);
    load_nvs_string(handle, "wx_location",  s_wx_location,  BUF_SIZE, CONFIG_SMART_FRIDGE_DEFAULT_WEATHER_LOCATION);
    load_nvs_string(handle, "wx_temp_path", s_wx_temp_path, BUF_SIZE, CONFIG_SMART_FRIDGE_DEFAULT_WEATHER_TEMP_PATH);
    load_nvs_string(handle, "wx_text_path", s_wx_text_path, BUF_SIZE, CONFIG_SMART_FRIDGE_DEFAULT_WEATHER_TEXT_PATH);

    nvs_close(handle);
    ESP_LOGI(TAG, "Credentials loaded successfully");
    return ESP_OK;
}

// ======== WiFi Credentials ========

const char* credentials_get_wifi_ssid(void) {
    return s_wifi_ssid;
}

const char* credentials_get_wifi_pass(void) {
    return s_wifi_pass;
}

esp_err_t credentials_set_wifi(const char* ssid, const char* pass) {
    esp_err_t err;

    err = save_nvs_string("wifi_ssid", ssid);
    if (err != ESP_OK) return err;

    err = save_nvs_string("wifi_pass", pass);
    if (err != ESP_OK) return err;

    // Update in-memory buffers
    strncpy(s_wifi_ssid, ssid, BUF_SIZE - 1);
    s_wifi_ssid[BUF_SIZE - 1] = '\0';
    strncpy(s_wifi_pass, pass, BUF_SIZE - 1);
    s_wifi_pass[BUF_SIZE - 1] = '\0';

    ESP_LOGI(TAG, "WiFi credentials updated (SSID: %s)", ssid);
    return ESP_OK;
}

// ======== LLM API Credentials ========

const char* credentials_get_llm_api_url(void) {
    return s_llm_url;
}

const char* credentials_get_llm_api_key(void) {
    return s_llm_key;
}

const char* credentials_get_llm_model(void) {
    return s_llm_model;
}

esp_err_t credentials_set_llm_api(const char* url, const char* key) {
    esp_err_t err;

    err = save_nvs_string("llm_url", url);
    if (err != ESP_OK) return err;

    err = save_nvs_string("llm_key", key);
    if (err != ESP_OK) return err;

    strncpy(s_llm_url, url, BUF_SIZE - 1);
    s_llm_url[BUF_SIZE - 1] = '\0';
    strncpy(s_llm_key, key, BUF_SIZE - 1);
    s_llm_key[BUF_SIZE - 1] = '\0';

    ESP_LOGI(TAG, "LLM API credentials updated (URL: %s)", url);
    return ESP_OK;
}

esp_err_t credentials_set_llm_model(const char* model) {
    esp_err_t err = save_nvs_string("llm_model", model);
    if (err != ESP_OK) return err;

    strncpy(s_llm_model, model, BUF_SIZE - 1);
    s_llm_model[BUF_SIZE - 1] = '\0';

    ESP_LOGI(TAG, "LLM model updated to: %s", model);
    return ESP_OK;
}

// ======== Weather API Config ========

const char* credentials_get_weather_url(void)      { return s_wx_url; }
const char* credentials_get_weather_key(void)      { return s_wx_key; }
const char* credentials_get_weather_city(void)     { return s_wx_city; }
const char* credentials_get_weather_location(void) { return s_wx_location; }
const char* credentials_get_weather_temp_path(void){ return s_wx_temp_path; }
const char* credentials_get_weather_text_path(void){ return s_wx_text_path; }

esp_err_t credentials_set_weather_url(const char* url) {
    esp_err_t err = save_nvs_string("wx_url", url);
    if (err != ESP_OK) return err;
    strncpy(s_wx_url, url, sizeof(s_wx_url) - 1);
    s_wx_url[sizeof(s_wx_url) - 1] = '\0';
    return ESP_OK;
}

esp_err_t credentials_set_weather_key(const char* key) {
    esp_err_t err = save_nvs_string("wx_key", key);
    if (err != ESP_OK) return err;
    strncpy(s_wx_key, key, BUF_SIZE - 1);
    s_wx_key[BUF_SIZE - 1] = '\0';
    return ESP_OK;
}

esp_err_t credentials_set_weather_city(const char* city) {
    esp_err_t err = save_nvs_string("wx_city", city);
    if (err != ESP_OK) return err;
    strncpy(s_wx_city, city, BUF_SIZE - 1);
    s_wx_city[BUF_SIZE - 1] = '\0';
    return ESP_OK;
}

esp_err_t credentials_set_weather_location(const char* location) {
    esp_err_t err = save_nvs_string("wx_location", location);
    if (err != ESP_OK) return err;
    strncpy(s_wx_location, location, BUF_SIZE - 1);
    s_wx_location[BUF_SIZE - 1] = '\0';
    return ESP_OK;
}

esp_err_t credentials_set_weather_temp_path(const char* path) {
    esp_err_t err = save_nvs_string("wx_temp_path", path);
    if (err != ESP_OK) return err;
    strncpy(s_wx_temp_path, path, BUF_SIZE - 1);
    s_wx_temp_path[BUF_SIZE - 1] = '\0';
    return ESP_OK;
}

esp_err_t credentials_set_weather_text_path(const char* path) {
    esp_err_t err = save_nvs_string("wx_text_path", path);
    if (err != ESP_OK) return err;
    strncpy(s_wx_text_path, path, BUF_SIZE - 1);
    s_wx_text_path[BUF_SIZE - 1] = '\0';
    return ESP_OK;
}
