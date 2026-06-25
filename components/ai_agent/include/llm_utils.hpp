#pragma once

#include <string>
#include <cstring>
#include <cstdlib>
#include "system_events.h"
#include "esp_log.h"

namespace smart_fridge {
namespace ai {

/**
 * @brief 安全拷贝字符串到固定大小缓冲区
 */
inline void safe_copy_text(char* dst, size_t dst_size, const std::string& src) {
    if (!dst || dst_size == 0) return;
    if (src.empty()) {
        dst[0] = '\0';
        return;
    }
    size_t copy_len = src.length();
    if (copy_len >= dst_size) copy_len = dst_size - 1;
    memcpy(dst, src.c_str(), copy_len);
    dst[copy_len] = '\0';
}

/**
 * @brief 投递 LLM 响应事件到系统总线
 */
inline void dispatch_llm_response_event(const std::string& tts_text,
                                        int ui_action_id,
                                        bool tts_already_queued = false) {
    llm_response_payload_t* payload = (llm_response_payload_t*)malloc(sizeof(llm_response_payload_t));
    if (!payload) {
        ESP_LOGE("LLMUtils", "Failed to allocate LLM response payload");
        return;
    }
    memset(payload, 0, sizeof(llm_response_payload_t));
    safe_copy_text(payload->tts_text, sizeof(payload->tts_text), tts_text);
    payload->ui_action_id = ui_action_id;
    payload->tts_already_queued = tts_already_queued;

    if (send_system_event(EVT_LLM_RESPONSE_READY, payload, sizeof(llm_response_payload_t)) != ESP_OK) {
        free(payload);
        ESP_LOGE("LLMUtils", "Failed to send EVT_LLM_RESPONSE_READY");
    } else {
        ESP_LOGI("LLMUtils", "EVT_LLM_RESPONSE_READY dispatched");
    }
}

inline void dispatch_llm_stream_text_event(const std::string& text) {
    auto* payload = (llm_stream_text_payload_t*)malloc(
        sizeof(llm_stream_text_payload_t));
    if (!payload) {
        return;
    }
    memset(payload, 0, sizeof(*payload));
    safe_copy_text(payload->text, sizeof(payload->text), text);
    if (send_system_event(EVT_LLM_STREAM_TEXT, payload, sizeof(*payload)) !=
        ESP_OK) {
        free(payload);
    }
}

} // namespace ai
} // namespace smart_fridge
