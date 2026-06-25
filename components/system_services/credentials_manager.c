#include "credentials_manager.h"
#include "sdkconfig.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "cJSON.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "CredsMgr";

#define NVS_NAMESPACE "creds"
#define BUF_SIZE 256
#define LLM_KEY_BUF_SIZE CONFIG_AI_LLM_KEY_BUF_SIZE

// Static buffers for credential storage
static char s_wifi_ssid[BUF_SIZE];
static char s_wifi_pass[BUF_SIZE];

// Weather API config
static char s_wx_url[512];
static char s_wx_key[BUF_SIZE];
static char s_wx_city[BUF_SIZE];
static char s_wx_location[BUF_SIZE];
static char s_wx_temp_path[BUF_SIZE];
static char s_wx_text_path[BUF_SIZE];

// Voice model configs (ASR / LLM / TTS)
static voice_model_config_t s_voice_configs[VOICE_MODEL_COUNT];
static voice_model_config_t s_voice_history[VOICE_MODEL_COUNT][VOICE_MODEL_HISTORY_MAX];
static int s_voice_history_count[VOICE_MODEL_COUNT];

static esp_err_t save_nvs_string(const char* key, const char* value);

static const char* voice_compact_prefix(voice_model_type_t type) {
    switch (type) {
        case VOICE_MODEL_ASR: return "va";
        case VOICE_MODEL_LLM: return "vl";
        case VOICE_MODEL_TTS: return "vt";
        case VOICE_MODEL_REALTIME: return "vr";
        default: return "vu";
    }
}

static void voice_current_key(voice_model_type_t type, char* key,
                              size_t key_size) {
    snprintf(key, key_size, "%sc", voice_compact_prefix(type));
}

static void voice_history_key(voice_model_type_t type, int index,
                              char* key, size_t key_size) {
    snprintf(key, key_size, "%sh%d", voice_compact_prefix(type), index);
}

static void trim_in_place(char* text) {
    if (!text || !text[0]) return;
    char* start = text;
    while (*start && isspace((unsigned char)*start)) ++start;
    char* end = start + strlen(start);
    while (end > start && isspace((unsigned char)end[-1])) --end;
    const size_t length = (size_t)(end - start);
    if (start != text) memmove(text, start, length);
    text[length] = '\0';
}

static void normalize_voice_config(voice_model_config_t* cfg) {
    if (!cfg) return;
    cfg->provider[sizeof(cfg->provider) - 1] = '\0';
    cfg->url[sizeof(cfg->url) - 1] = '\0';
    cfg->key[sizeof(cfg->key) - 1] = '\0';
    cfg->model[sizeof(cfg->model) - 1] = '\0';
    cfg->extra[sizeof(cfg->extra) - 1] = '\0';
    trim_in_place(cfg->provider);
    trim_in_place(cfg->url);
    trim_in_place(cfg->key);
    trim_in_place(cfg->model);
    trim_in_place(cfg->extra);
}

static char* serialize_voice_config(const voice_model_config_t* cfg) {
    if (!cfg) return NULL;
    cJSON* root = cJSON_CreateObject();
    if (!root) return NULL;
    cJSON_AddStringToObject(root, "p", cfg->provider);
    cJSON_AddStringToObject(root, "u", cfg->url);
    cJSON_AddStringToObject(root, "k", cfg->key);
    cJSON_AddStringToObject(root, "m", cfg->model);
    cJSON_AddStringToObject(root, "x", cfg->extra);
    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

static bool copy_json_string(cJSON* root, const char* name, char* target,
                             size_t target_size) {
    cJSON* item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsString(item) || !item->valuestring) return false;
    strncpy(target, item->valuestring, target_size - 1);
    target[target_size - 1] = '\0';
    return true;
}

static esp_err_t deserialize_voice_config(const char* json,
                                          voice_model_config_t* cfg) {
    if (!json || !cfg) return ESP_ERR_INVALID_ARG;
    cJSON* root = cJSON_Parse(json);
    if (!root) return ESP_ERR_INVALID_RESPONSE;
    voice_model_config_t parsed = {0};
    const bool valid =
        copy_json_string(root, "p", parsed.provider,
                         sizeof(parsed.provider)) &&
        copy_json_string(root, "u", parsed.url, sizeof(parsed.url)) &&
        copy_json_string(root, "k", parsed.key, sizeof(parsed.key)) &&
        copy_json_string(root, "m", parsed.model, sizeof(parsed.model)) &&
        copy_json_string(root, "x", parsed.extra, sizeof(parsed.extra));
    cJSON_Delete(root);
    if (!valid) return ESP_ERR_INVALID_RESPONSE;
    normalize_voice_config(&parsed);
    *cfg = parsed;
    return ESP_OK;
}

static esp_err_t load_voice_config_json(nvs_handle_t handle,
                                        const char* key,
                                        voice_model_config_t* cfg) {
    size_t length = 0;
    esp_err_t err = nvs_get_str(handle, key, NULL, &length);
    if (err != ESP_OK) return err;
    char* json = (char*)malloc(length);
    if (!json) return ESP_ERR_NO_MEM;
    err = nvs_get_str(handle, key, json, &length);
    if (err == ESP_OK) err = deserialize_voice_config(json, cfg);
    free(json);
    return err;
}

static esp_err_t save_voice_config_json(const char* key,
                                        const voice_model_config_t* cfg) {
    char* json = serialize_voice_config(cfg);
    if (!json) return ESP_ERR_NO_MEM;
    esp_err_t err = save_nvs_string(key, json);
    free(json);
    return err;
}

static void erase_nvs_key(const char* key) {
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return;
    esp_err_t err = nvs_erase_key(handle, key);
    if (err == ESP_OK) nvs_commit(handle);
    nvs_close(handle);
}

/**
 * @brief Helper: read a string from NVS. If key not found, use fallback default.
 */
static esp_err_t load_nvs_string(nvs_handle_t handle, const char* key,
                                  char* buf, size_t buf_size,
                                  const char* default_val) {
    size_t required_size = buf_size;
    esp_err_t err = nvs_get_str(handle, key, buf, &required_size);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        strncpy(buf, default_val, buf_size - 1);
        buf[buf_size - 1] = '\0';
        ESP_LOGI(TAG, "Key '%s' not in NVS, using default", key);
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

/**
 * @brief Helper: read a blob from NVS.
 */
static esp_err_t load_nvs_blob(nvs_handle_t handle, const char* key,
                                void* buf, size_t buf_size) {
    size_t required_size = buf_size;
    esp_err_t err = nvs_get_blob(handle, key, buf, &required_size);
    if (err != ESP_OK) {
        return err;
    }
    if (required_size != buf_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static const char* voice_type_prefix(voice_model_type_t type) {
    switch (type) {
        case VOICE_MODEL_ASR: return "voice_asr";
        case VOICE_MODEL_LLM: return "voice_llm";
        case VOICE_MODEL_TTS: return "voice_tts";
        case VOICE_MODEL_REALTIME: return "voice_rt";
        default: return "voice_unk";
    }
}

static void config_set_default(voice_model_config_t* cfg,
                                const char* provider,
                                const char* url,
                                const char* key,
                                const char* model,
                                const char* extra) {
    strncpy(cfg->provider, provider ? provider : "", sizeof(cfg->provider) - 1);
    cfg->provider[sizeof(cfg->provider) - 1] = '\0';
    strncpy(cfg->url, url ? url : "", sizeof(cfg->url) - 1);
    cfg->url[sizeof(cfg->url) - 1] = '\0';
    strncpy(cfg->key, key ? key : "", sizeof(cfg->key) - 1);
    cfg->key[sizeof(cfg->key) - 1] = '\0';
    strncpy(cfg->model, model ? model : "", sizeof(cfg->model) - 1);
    cfg->model[sizeof(cfg->model) - 1] = '\0';
    strncpy(cfg->extra, extra ? extra : "", sizeof(cfg->extra) - 1);
    cfg->extra[sizeof(cfg->extra) - 1] = '\0';
    normalize_voice_config(cfg);
}

static bool config_is_empty(const voice_model_config_t* cfg) {
    return !cfg || (strlen(cfg->url) == 0 && strlen(cfg->key) == 0 && strlen(cfg->model) == 0);
}

static bool config_is_equal(const voice_model_config_t* a, const voice_model_config_t* b) {
    if (!a || !b) return false;
    return strcmp(a->provider, b->provider) == 0 &&
           strcmp(a->url, b->url) == 0 &&
           strcmp(a->key, b->key) == 0 &&
           strcmp(a->model, b->model) == 0 &&
           strcmp(a->extra, b->extra) == 0;
}

static bool key_should_be_preserved(const char* key) {
    return !key || key[0] == '\0' ||
           strcmp(key, "********") == 0 ||
           strcmp(key, "sk-********") == 0;
}

static void load_voice_model_defaults(void) {
    config_set_default(&s_voice_configs[VOICE_MODEL_LLM],
                       CONFIG_SMART_FRIDGE_DEFAULT_LLM_PROVIDER,
                       CONFIG_SMART_FRIDGE_DEFAULT_LLM_API_URL,
                       CONFIG_SMART_FRIDGE_DEFAULT_LLM_API_KEY,
                       CONFIG_SMART_FRIDGE_DEFAULT_LLM_MODEL,
                       CONFIG_SMART_FRIDGE_DEFAULT_LLM_EXTRA);

    config_set_default(&s_voice_configs[VOICE_MODEL_ASR],
                       CONFIG_SMART_FRIDGE_DEFAULT_ASR_PROVIDER,
                       CONFIG_SMART_FRIDGE_DEFAULT_ASR_URL,
                       CONFIG_SMART_FRIDGE_DEFAULT_ASR_KEY,
                       CONFIG_SMART_FRIDGE_DEFAULT_ASR_MODEL,
                       CONFIG_SMART_FRIDGE_DEFAULT_ASR_EXTRA);

    config_set_default(&s_voice_configs[VOICE_MODEL_TTS],
                       CONFIG_SMART_FRIDGE_DEFAULT_TTS_PROVIDER,
                       CONFIG_SMART_FRIDGE_DEFAULT_TTS_URL,
                       CONFIG_SMART_FRIDGE_DEFAULT_TTS_KEY,
                       CONFIG_SMART_FRIDGE_DEFAULT_TTS_MODEL,
                       CONFIG_SMART_FRIDGE_DEFAULT_TTS_EXTRA);

    config_set_default(&s_voice_configs[VOICE_MODEL_REALTIME],
                       CONFIG_SMART_FRIDGE_DEFAULT_REALTIME_PROVIDER,
                       CONFIG_SMART_FRIDGE_DEFAULT_REALTIME_URL,
                       CONFIG_SMART_FRIDGE_DEFAULT_REALTIME_KEY,
                       CONFIG_SMART_FRIDGE_DEFAULT_REALTIME_MODEL,
                       CONFIG_SMART_FRIDGE_DEFAULT_REALTIME_EXTRA);

    for (int i = 0; i < VOICE_MODEL_COUNT; i++) {
        s_voice_history_count[i] = 0;
        memset(s_voice_history[i], 0, sizeof(s_voice_history[i]));
    }
}

static void load_voice_models_from_nvs(nvs_handle_t handle) {
    char cur_key[32];
    char hist_key[32];

    for (int t = 0; t < VOICE_MODEL_COUNT; t++) {
        voice_model_type_t type = (voice_model_type_t)t;
        voice_current_key(type, cur_key, sizeof(cur_key));
        esp_err_t load_err =
            load_voice_config_json(handle, cur_key, &s_voice_configs[t]);
        if (load_err == ESP_OK) {
            // A previous migration may have left the large legacy blob behind.
            char legacy_key[32];
            snprintf(legacy_key, sizeof(legacy_key), "%s_cur",
                     voice_type_prefix(type));
            erase_nvs_key(legacy_key);
        } else {
            // Backward compatibility with the former raw-struct blob.
            char legacy_key[32];
            snprintf(legacy_key, sizeof(legacy_key), "%s_cur",
                     voice_type_prefix(type));
            load_err = load_nvs_blob(
                handle, legacy_key, &s_voice_configs[t],
                sizeof(voice_model_config_t));
            if (load_err == ESP_OK) {
                normalize_voice_config(&s_voice_configs[t]);
                ESP_LOGI(TAG, "Migrating legacy voice config '%s'",
                         legacy_key);
                if (save_voice_config_json(cur_key,
                                           &s_voice_configs[t]) == ESP_OK) {
                    erase_nvs_key(legacy_key);
                }
            } else {
                ESP_LOGI(TAG,
                         "Voice config '%s' not in NVS, using default",
                         cur_key);
            }
        }

        s_voice_history_count[t] = 0;
        for (int h = 0; h < VOICE_MODEL_HISTORY_MAX; h++) {
            voice_history_key(type, h, hist_key, sizeof(hist_key));
            if (load_voice_config_json(handle, hist_key,
                                       &s_voice_history[t][h]) == ESP_OK) {
                s_voice_history_count[t] = h + 1;
            } else {
                break;
            }
        }
        ESP_LOGI(TAG,
                 "Loaded %s config (provider=%s, model=%s, key=%s, history=%d)",
                 voice_type_prefix(type), s_voice_configs[t].provider,
                 s_voice_configs[t].model,
                 s_voice_configs[t].key[0] ? "configured" : "empty",
                 s_voice_history_count[t]);
    }
}

static void migrate_legacy_streaming_voice_config(void) {
    voice_model_config_t asr = s_voice_configs[VOICE_MODEL_ASR];
    voice_model_config_t tts = s_voice_configs[VOICE_MODEL_TTS];
    bool changed = false;

    if (strcmp(asr.provider, "volcengine") == 0 &&
        strstr(asr.url, "/api/v1/acs") != NULL) {
        strncpy(asr.url,
                "wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async",
                sizeof(asr.url) - 1);
        asr.url[sizeof(asr.url) - 1] = '\0';
        changed = true;
    }
    if (strcmp(asr.provider, "volcengine") == 0 &&
        strcmp(asr.url,
               "wss://openspeech.bytedance.com/api/v3/sauc/bigmodel") == 0) {
        strncpy(asr.url,
                "wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async",
                sizeof(asr.url) - 1);
        asr.url[sizeof(asr.url) - 1] = '\0';
        changed = true;
    }
    if (strcmp(asr.provider, "volcengine") == 0 &&
        (asr.model[0] == '\0' ||
         strcmp(asr.model, "volcengine-asr") == 0)) {
        strncpy(asr.model, "volc.bigasr.sauc.duration",
                sizeof(asr.model) - 1);
        asr.model[sizeof(asr.model) - 1] = '\0';
        changed = true;
    }

    if (changed) {
        ESP_LOGI(TAG, "Migrating legacy batch ASR config to WebSocket ASR");
        credentials_set_voice_model_config(VOICE_MODEL_ASR, &asr);
    }

    changed = false;
    if (strcmp(tts.provider, "volcengine") == 0 &&
        strstr(tts.url, "/api/v1/tts") != NULL) {
        strncpy(tts.url,
                "wss://openspeech.bytedance.com/api/v3/tts/bidirection",
                sizeof(tts.url) - 1);
        tts.url[sizeof(tts.url) - 1] = '\0';
        changed = true;
    }
    if (strcmp(tts.model, "volcengine-tts") == 0 ||
        tts.model[0] == '\0') {
        strncpy(tts.model, "zh_female_vv_jupiter_bigtts",
                sizeof(tts.model) - 1);
        tts.model[sizeof(tts.model) - 1] = '\0';
        changed = true;
    }
    if (tts.extra[0] == '\0' || tts.extra[0] != '{') {
        strncpy(tts.extra,
                "{\"resource_id\":\"seed-tts-2.0\",\"model\":\"seed-tts-2.0-standard\",\"sample_rate\":16000}",
                sizeof(tts.extra) - 1);
        tts.extra[sizeof(tts.extra) - 1] = '\0';
        changed = true;
    }
    if (changed) {
        ESP_LOGI(TAG, "Migrating legacy HTTP TTS config to WebSocket TTS");
        credentials_set_voice_model_config(VOICE_MODEL_TTS, &tts);
    }
}

static esp_err_t save_voice_model_current(voice_model_type_t type) {
    char key[32];
    voice_current_key(type, key, sizeof(key));
    esp_err_t err = save_voice_config_json(key, &s_voice_configs[type]);
    if (err != ESP_OK) return err;

    nvs_handle_t handle;
    err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;
    voice_model_config_t verified = {0};
    err = load_voice_config_json(handle, key, &verified);
    nvs_close(handle);
    if (err != ESP_OK) return err;
    return config_is_equal(&verified, &s_voice_configs[type])
               ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static esp_err_t save_voice_model_history(voice_model_type_t type) {
    char key[32];
    esp_err_t err = ESP_OK;
    for (int h = 0; h < VOICE_MODEL_HISTORY_MAX; h++) {
        voice_history_key(type, h, key, sizeof(key));
        if (h < s_voice_history_count[type]) {
            err = save_voice_config_json(key, &s_voice_history[type][h]);
        } else {
            erase_nvs_key(key);
        }
        if (err != ESP_OK) break;
    }
    return err;
}

static void push_voice_model_history(voice_model_type_t type, const voice_model_config_t* cfg) {
    if (!cfg || config_is_empty(cfg)) return;

    // 如果历史顶部已经相同，不再重复入栈
    if (s_voice_history_count[type] > 0 &&
        config_is_equal(&s_voice_history[type][0], cfg)) {
        return;
    }

    // 后移历史
    for (int h = VOICE_MODEL_HISTORY_MAX - 1; h > 0; h--) {
        s_voice_history[type][h] = s_voice_history[type][h - 1];
    }
    s_voice_history[type][0] = *cfg;
    if (s_voice_history_count[type] < VOICE_MODEL_HISTORY_MAX) {
        s_voice_history_count[type]++;
    }
}

esp_err_t credentials_init(void) {
    ESP_LOGI(TAG, "Initializing credentials manager...");

    load_voice_model_defaults();

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "NVS namespace '%s' not found, using all Kconfig defaults", NVS_NAMESPACE);
        strncpy(s_wifi_ssid, CONFIG_SMART_FRIDGE_DEFAULT_WIFI_SSID, BUF_SIZE - 1);
        s_wifi_ssid[BUF_SIZE - 1] = '\0';
        strncpy(s_wifi_pass, CONFIG_SMART_FRIDGE_DEFAULT_WIFI_PASS, BUF_SIZE - 1);
        s_wifi_pass[BUF_SIZE - 1] = '\0';
        strncpy(s_wx_url, CONFIG_SMART_FRIDGE_DEFAULT_WEATHER_URL, sizeof(s_wx_url) - 1);
        s_wx_url[sizeof(s_wx_url) - 1] = '\0';
        strncpy(s_wx_key, CONFIG_SMART_FRIDGE_DEFAULT_WEATHER_KEY, BUF_SIZE - 1);
        s_wx_key[BUF_SIZE - 1] = '\0';
        strncpy(s_wx_city, CONFIG_SMART_FRIDGE_DEFAULT_WEATHER_CITY, BUF_SIZE - 1);
        s_wx_city[BUF_SIZE - 1] = '\0';
        strncpy(s_wx_location, CONFIG_SMART_FRIDGE_DEFAULT_WEATHER_LOCATION, BUF_SIZE - 1);
        s_wx_location[BUF_SIZE - 1] = '\0';
        strncpy(s_wx_temp_path, CONFIG_SMART_FRIDGE_DEFAULT_WEATHER_TEMP_PATH, BUF_SIZE - 1);
        s_wx_temp_path[BUF_SIZE - 1] = '\0';
        strncpy(s_wx_text_path, CONFIG_SMART_FRIDGE_DEFAULT_WEATHER_TEXT_PATH, BUF_SIZE - 1);
        s_wx_text_path[BUF_SIZE - 1] = '\0';
        return ESP_OK;
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace '%s': %s", NVS_NAMESPACE, esp_err_to_name(err));
        return err;
    }

    // Load each credential from NVS, falling back to Kconfig defaults
    load_nvs_string(handle, "wifi_ssid", s_wifi_ssid, BUF_SIZE, CONFIG_SMART_FRIDGE_DEFAULT_WIFI_SSID);
    load_nvs_string(handle, "wifi_pass", s_wifi_pass, BUF_SIZE, CONFIG_SMART_FRIDGE_DEFAULT_WIFI_PASS);

    load_voice_models_from_nvs(handle);

    load_nvs_string(handle, "wx_url",       s_wx_url,       sizeof(s_wx_url), CONFIG_SMART_FRIDGE_DEFAULT_WEATHER_URL);
    load_nvs_string(handle, "wx_key",       s_wx_key,       BUF_SIZE, CONFIG_SMART_FRIDGE_DEFAULT_WEATHER_KEY);
    load_nvs_string(handle, "wx_city",      s_wx_city,      BUF_SIZE, CONFIG_SMART_FRIDGE_DEFAULT_WEATHER_CITY);
    load_nvs_string(handle, "wx_location",  s_wx_location,  BUF_SIZE, CONFIG_SMART_FRIDGE_DEFAULT_WEATHER_LOCATION);
    load_nvs_string(handle, "wx_temp_path", s_wx_temp_path, BUF_SIZE, CONFIG_SMART_FRIDGE_DEFAULT_WEATHER_TEMP_PATH);
    load_nvs_string(handle, "wx_text_path", s_wx_text_path, BUF_SIZE, CONFIG_SMART_FRIDGE_DEFAULT_WEATHER_TEXT_PATH);

    nvs_close(handle);
    migrate_legacy_streaming_voice_config();
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

    strncpy(s_wifi_ssid, ssid, BUF_SIZE - 1);
    s_wifi_ssid[BUF_SIZE - 1] = '\0';
    strncpy(s_wifi_pass, pass, BUF_SIZE - 1);
    s_wifi_pass[BUF_SIZE - 1] = '\0';

    ESP_LOGI(TAG, "WiFi credentials updated (SSID: %s)", ssid);
    return ESP_OK;
}

// ======== Voice Model Configs ========

const voice_model_config_t* credentials_get_voice_model_config(voice_model_type_t type) {
    if (type < 0 || type >= VOICE_MODEL_COUNT) return NULL;
    return &s_voice_configs[type];
}

int credentials_get_voice_model_history_count(voice_model_type_t type) {
    if (type < 0 || type >= VOICE_MODEL_COUNT) return 0;
    return s_voice_history_count[type];
}

const voice_model_config_t* credentials_get_voice_model_history(voice_model_type_t type, int index) {
    if (type < 0 || type >= VOICE_MODEL_COUNT) return NULL;
    if (index < 0 || index >= s_voice_history_count[type]) return NULL;
    return &s_voice_history[type][index];
}

esp_err_t credentials_set_voice_model_config(voice_model_type_t type, const voice_model_config_t* cfg) {
    if (type < 0 || type >= VOICE_MODEL_COUNT || !cfg) {
        return ESP_ERR_INVALID_ARG;
    }

    voice_model_config_t merged = *cfg;
    normalize_voice_config(&merged);
    if (key_should_be_preserved(merged.key)) {
        strncpy(merged.key, s_voice_configs[type].key, sizeof(merged.key) - 1);
        merged.key[sizeof(merged.key) - 1] = '\0';
    }

    voice_model_config_t previous = s_voice_configs[type];

    // 先持久化当前配置；只有 NVS commit 成功后才更新 RAM，避免出现
    // “页面看似保存成功、重启后丢失”的假成功状态。
    s_voice_configs[type] = merged;
    esp_err_t err = save_voice_model_current(type);
    if (err != ESP_OK) {
        s_voice_configs[type] = previous;
        ESP_LOGE(TAG, "Failed to persist %s config: %s",
                 voice_type_prefix(type), esp_err_to_name(err));
        return err;
    }
    char legacy_key[32];
    snprintf(legacy_key, sizeof(legacy_key), "%s_cur",
             voice_type_prefix(type));
    erase_nvs_key(legacy_key);

    // 旧配置入历史（如果存在且与新配置不同）
    if (!config_is_empty(&previous) &&
        !config_is_equal(&previous, &merged)) {
        push_voice_model_history(type, &previous);
        const esp_err_t history_err = save_voice_model_history(type);
        if (history_err != ESP_OK) {
            ESP_LOGW(TAG, "Current config saved, history save failed: %s",
                     esp_err_to_name(history_err));
        }
    }

    ESP_LOGI(TAG, "%s config persisted (provider=%s, model=%s, key=%s)",
             voice_type_prefix(type), merged.provider, merged.model,
             merged.key[0] ? "configured" : "empty");
    return ESP_OK;
}

esp_err_t credentials_delete_voice_model_history(voice_model_type_t type, int index) {
    if (type < 0 || type >= VOICE_MODEL_COUNT) return ESP_ERR_INVALID_ARG;
    if (index < 0 || index >= s_voice_history_count[type]) return ESP_ERR_INVALID_ARG;

    // 后面的历史前移
    for (int h = index; h < s_voice_history_count[type] - 1; h++) {
        s_voice_history[type][h] = s_voice_history[type][h + 1];
    }
    memset(&s_voice_history[type][s_voice_history_count[type] - 1],
           0, sizeof(voice_model_config_t));
    s_voice_history_count[type]--;

    return save_voice_model_history(type);
}

// ======== LLM Compatibility APIs ========

const char* credentials_get_llm_api_url(void) {
    return s_voice_configs[VOICE_MODEL_LLM].url;
}

const char* credentials_get_llm_api_key(void) {
    return s_voice_configs[VOICE_MODEL_LLM].key;
}

const char* credentials_get_llm_model(void) {
    return s_voice_configs[VOICE_MODEL_LLM].model;
}

esp_err_t credentials_set_llm_api(const char* url, const char* key) {
    voice_model_config_t cfg = s_voice_configs[VOICE_MODEL_LLM];
    if (url) {
        strncpy(cfg.url, url, sizeof(cfg.url) - 1);
        cfg.url[sizeof(cfg.url) - 1] = '\0';
    }
    if (!key_should_be_preserved(key)) {
        strncpy(cfg.key, key, sizeof(cfg.key) - 1);
        cfg.key[sizeof(cfg.key) - 1] = '\0';
    }
    return credentials_set_voice_model_config(VOICE_MODEL_LLM, &cfg);
}

esp_err_t credentials_set_llm_model(const char* model) {
    voice_model_config_t cfg = s_voice_configs[VOICE_MODEL_LLM];
    strncpy(cfg.model, model, sizeof(cfg.model) - 1);
    cfg.model[sizeof(cfg.model) - 1] = '\0';
    return credentials_set_voice_model_config(VOICE_MODEL_LLM, &cfg);
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
