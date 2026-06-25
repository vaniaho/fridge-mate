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

// ======== Voice Model Config (ASR / LLM / TTS) ========

#define VOICE_MODEL_PROVIDER_MAX_LEN 64
#define VOICE_MODEL_URL_MAX_LEN      256
#define VOICE_MODEL_KEY_MAX_LEN      512
#define VOICE_MODEL_NAME_MAX_LEN     128
#define VOICE_MODEL_EXTRA_MAX_LEN    256
#define VOICE_MODEL_HISTORY_MAX      5

/**
 * @brief 语音链路中单模型配置
 * 用于 ASR、LLM、TTS 三环节，provider 可取 "volcengine"/"baidu"/"moonshot"/"deepseek"/...
 */
typedef struct {
    char provider[VOICE_MODEL_PROVIDER_MAX_LEN];
    char url[VOICE_MODEL_URL_MAX_LEN];
    char key[VOICE_MODEL_KEY_MAX_LEN];
    char model[VOICE_MODEL_NAME_MAX_LEN];
    char extra[VOICE_MODEL_EXTRA_MAX_LEN];  /**< 扩展字段：TTS voice_id、ASR format 等 */
} voice_model_config_t;

typedef enum {
    VOICE_MODEL_ASR = 0,
    VOICE_MODEL_LLM,
    VOICE_MODEL_TTS,
    VOICE_MODEL_REALTIME,
    VOICE_MODEL_COUNT
} voice_model_type_t;

const voice_model_config_t* credentials_get_voice_model_config(voice_model_type_t type);
int credentials_get_voice_model_history_count(voice_model_type_t type);
const voice_model_config_t* credentials_get_voice_model_history(voice_model_type_t type, int index);

/**
 * @brief 设置当前激活的语音模型配置
 * 旧配置会被推入历史记录（最多保留 VOICE_MODEL_HISTORY_MAX 条），只有手动删除才会移除。
 * cfg->key 为空或为脱敏占位值时保留当前 API Key，避免编辑其他字段时误清空密钥。
 */
esp_err_t credentials_set_voice_model_config(voice_model_type_t type, const voice_model_config_t* cfg);

/**
 * @brief 删除某条历史配置
 * @param index 历史索引（0 为最近一条历史）
 */
esp_err_t credentials_delete_voice_model_history(voice_model_type_t type, int index);

// ======== 兼容旧接口：从 LLM 配置中返回 ========
const char* credentials_get_llm_api_url(void);
const char* credentials_get_llm_api_key(void);
const char* credentials_get_llm_model(void);
esp_err_t credentials_set_llm_api(const char* url, const char* key);
esp_err_t credentials_set_llm_model(const char* model);

// ======== Weather API Config ========
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
