#include "ai_agent.hpp"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "credentials_manager.h"
#include <string.h>

static const char *TAG = "LLMClient";

namespace smart_fridge {
namespace ai {

// HTTP 响应回调函数：用来拼接流式返回的 body 数据
esp_err_t _http_event_handler(esp_http_client_event_t *evt) {
    std::string *response_buffer = (std::string *)evt->user_data;
    switch(evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            response_buffer->append((char*)evt->data, evt->data_len);
            break;
        default:
            break;
    }
    return ESP_OK;
}

bool call_llm_api(const std::string& user_voice_text, std::string& out_reply) {
    ESP_LOGI(TAG, "Calling LLM with user input: %s", user_voice_text.c_str());
    
    // 1. 获取动态组装的包含库存状态的请求体
    std::string request_body = build_llm_request(user_voice_text);
    std::string response_buffer;

    // 2. 从凭证管理器获取 API URL 和 Key（NVS 存储，支持 GUI 运行时修改）
    const char* api_url = credentials_get_llm_api_url();
    const char* api_key = credentials_get_llm_api_key();

    if (strlen(api_key) == 0) {
        ESP_LOGE(TAG, "LLM API Key is empty! Please configure via 'idf.py menuconfig' or GUI settings.");
        return false;
    }

    esp_http_client_config_t config = {};
    config.url = api_url;
    config.event_handler = _http_event_handler;
    config.user_data = &response_buffer;
    config.timeout_ms = 60000; // LLM 生成可能较慢，60 秒兜底
    config.crt_bundle_attach = esp_crt_bundle_attach; // 使用内置根证书包验证 HTTPS

    // 3. 初始化 HTTP 客户端并配置 Header
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    
    char auth_header[256];  // 栈变量，避免 static 在多线程调用时被覆盖
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", api_key);
    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_post_field(client, request_body.c_str(), request_body.length());

    // 4. 执行请求
    esp_err_t err;
    do {
        err = esp_http_client_perform(client);
    } while (err == ESP_ERR_HTTP_EAGAIN);

    bool success = false;

    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "HTTP POST Status = %d, response length = %d",
                 status_code, response_buffer.length());
        
        if (status_code == 200) {
            // 请求成功，将返回的 JSON 丢给解析器处理
            out_reply = parse_and_execute_llm_response(response_buffer);
            success = true;
        } else {
            ESP_LOGE(TAG, "HTTP Error Status: %d", status_code);
            ESP_LOGE(TAG, "Error Response: %s", response_buffer.c_str());
        }
    } else {
        ESP_LOGE(TAG, "HTTP POST request failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    return success;
}

} // namespace ai
} // namespace smart_fridge
