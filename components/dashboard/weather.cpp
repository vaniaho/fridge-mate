#include "weather.hpp"
#include "credentials_manager.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string>
#include <cstring>
#include <cstdlib>
#include <zlib.h>

static const char *TAG = "Weather";

namespace smart_fridge {
namespace dashboard {

static WeatherInfo s_cache = { 0, "", "", 0, false };
static SemaphoreHandle_t s_mutex = NULL;
static bool s_initialized = false;

// 移除 http_event_handler，改用手动 read 获取解压后数据

/**
 * @brief 把 URL 模板中的 {location} / {key} 替换为实际值
 */
static std::string build_url(const char *tmpl,
                             const char *location,
                             const char *key) {
    std::string url(tmpl);
    const struct { const char *token; const char *val; } subs[] = {
        { "{location}", location },
        { "{key}",      key      },
    };
    for (auto& s : subs) {
        size_t pos = 0;
        while ((pos = url.find(s.token, pos)) != std::string::npos) {
            url.replace(pos, strlen(s.token), s.val ? s.val : "");
            pos += strlen(s.val);
        }
    }
    return url;
}

/**
 * @brief cJSON 字段路径遍历器：按 "." 分隔逐层取对象，取叶子节点的字符串值。
 *        例如 path="now.temp" 在 {"now":{"temp":"25"}} 中返回 "25"。
 *        兼容整型/浮点型叶子（转为字符串）。
 * @return 字段值的字符串形式，未命中返回空串
 */
static std::string json_extract_str(const cJSON *root, const char *path) {
    if (!root || !path) return "";

    // 复制 path 以便 strtok_r 切分
    char buf[128];
    strncpy(buf, path, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    const cJSON *node = root;
    char *saveptr = NULL;
    char *tok = strtok_r(buf, ".", &saveptr);
    while (tok != NULL) {
        if (!node) return "";
        node = cJSON_GetObjectItem(node, tok);
        tok = strtok_r(NULL, ".", &saveptr);
    }
    if (!node) return "";

    // 叶子节点可能是 string / number
    if (cJSON_IsString(node)) return node->valuestring ? node->valuestring : "";
    if (cJSON_IsNumber(node)) {
        char num[32];
        snprintf(num, sizeof(num), "%g", node->valuedouble);
        return num;
    }
    return "";
}

/**
 * @brief 手动解压 GZIP 数据
 */
static std::string decompress_gzip(const std::string& compressed) {
    if (compressed.empty()) return "";
    
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    
    // 16 + MAX_WBITS 使能 GZIP 头部解析
    if (inflateInit2(&zs, 16 + 15) != Z_OK) {
        return "";
    }
    
    zs.next_in = (Bytef*)compressed.data();
    zs.avail_in = compressed.size();
    
    int ret;
    char outbuffer[1024];
    std::string outstring;
    
    do {
        zs.next_out = reinterpret_cast<Bytef*>(outbuffer);
        zs.avail_out = sizeof(outbuffer);
        
        ret = inflate(&zs, Z_NO_FLUSH);
        
        if (outstring.size() < zs.total_out) {
            outstring.append(outbuffer, zs.total_out - outstring.size());
        }
    } while (ret == Z_OK);
    
    inflateEnd(&zs);
    
    if (ret != Z_STREAM_END) {
        ESP_LOGE(TAG, "GZIP decompression failed: ret=%d", ret);
        return "";
    }
    
    return outstring;
}

void weather_init() {
    if (s_initialized) return;
    s_mutex = xSemaphoreCreateMutex();
    s_initialized = true;
    ESP_LOGI(TAG, "Weather module initialized. (URL configured: %s)",
             strlen(credentials_get_weather_url()) > 0 ? "yes" : "no");
}

WeatherInfo weather_get() {
    WeatherInfo copy;
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
    copy = s_cache;
    if (s_mutex) xSemaphoreGive(s_mutex);
    return copy;
}

bool weather_refresh() {
    const char *url_tmpl = credentials_get_weather_url();
    const char *key = credentials_get_weather_key();
    const char *location = credentials_get_weather_location();

    // 配置不齐全则跳过
    if (!url_tmpl || strlen(url_tmpl) == 0 ||
        !key || strlen(key) == 0 ||
        !location || strlen(location) == 0) {
        ESP_LOGW(TAG, "Weather config incomplete, skipping refresh");
        return false;
    }

    std::string url = build_url(url_tmpl, location, key);
    std::string response;

    ESP_LOGI(TAG, "Fetching weather: %s", url.c_str());

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.timeout_ms = 15000;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.disable_auto_redirect = false;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_method(client, HTTP_METHOD_GET);
    esp_http_client_set_header(client, "Accept-Encoding", "gzip");

    esp_err_t err;
    do {
        err = esp_http_client_open(client, 0);
    } while (err == ESP_ERR_HTTP_EAGAIN);

    bool success = false;
    if (err == ESP_OK) {
        esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        
        char buffer[256];
        int read_len;
        while ((read_len = esp_http_client_read(client, buffer, sizeof(buffer))) > 0) {
            response.append(buffer, read_len);
        }

        ESP_LOGI(TAG, "Weather HTTP status=%d, len=%d", status, (int)response.length());
        if (status == 200) {
            // 如果数据以 gzip 魔数 (0x1F 0x8B) 开头，则进行手动解压
            if (response.length() >= 2 && (uint8_t)response[0] == 0x1F && (uint8_t)response[1] == 0x8B) {
                ESP_LOGI(TAG, "Detected GZIP data, decompressing manually...");
                std::string decompressed = decompress_gzip(response);
                if (!decompressed.empty()) {
                    response = decompressed;
                    ESP_LOGI(TAG, "Decompressed length: %d", (int)response.length());
                } else {
                    ESP_LOGE(TAG, "Manual decompression failed!");
                }
            }

            cJSON *root = cJSON_Parse(response.c_str());
            if (root) {
                std::string temp_str = json_extract_str(root, credentials_get_weather_temp_path());
                std::string text_str = json_extract_str(root, credentials_get_weather_text_path());

                if (!temp_str.empty() || !text_str.empty()) {
                    WeatherInfo info;
                    info.temp = temp_str.empty() ? 0.0f : strtof(temp_str.c_str(), nullptr);
                    info.text = text_str;
                    info.city = credentials_get_weather_city();
                    time(&info.updated);
                    info.valid = true;

                    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
                    s_cache = info;
                    if (s_mutex) xSemaphoreGive(s_mutex);

                    ESP_LOGI(TAG, "Weather updated: %s %.1f°C %s",
                             info.city.c_str(), info.temp, info.text.c_str());
                    success = true;
                } else {
                    ESP_LOGE(TAG, "Weather fields not found by path (temp=%s text=%s)",
                             credentials_get_weather_temp_path(),
                             credentials_get_weather_text_path());
                }
                cJSON_Delete(root);
            } else {
                ESP_LOGE(TAG, "Failed to parse weather JSON: %.200s", response.c_str());
            }
        } else {
            ESP_LOGE(TAG, "Weather HTTP error status=%d: %.200s", status, response.c_str());
        }
    } else {
        ESP_LOGE(TAG, "Weather HTTP request failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    return success;
}

} // namespace dashboard
} // namespace smart_fridge
