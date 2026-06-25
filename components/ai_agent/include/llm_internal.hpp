#pragma once

#include <string>
#include <functional>
#include "llm_types.hpp"

namespace smart_fridge {
namespace ai {

enum class pending_confirmation_result_t {
    NOT_HANDLED = 0,
    HANDLED
};

/**
 * @brief 执行原始 HTTP POST 请求到 LLM API
 * @param request_body 请求体 JSON
 * @param out_raw_response 原始响应字符串
 * @param out_http_status HTTP 状态码
 * @return 错误码
 */
llm_error_t http_perform_llm(const std::string& request_body,
                             std::string& out_raw_response,
                             int& out_http_status);

struct llm_stream_callbacks_t {
    std::function<bool(const std::string& delta)> on_delta;
    std::function<bool(void)> is_cancelled;
};

llm_error_t http_perform_llm_stream(
    const std::string& request_body,
    const llm_stream_callbacks_t& callbacks,
    std::string& out_content,
    int& out_http_status);

/**
 * @brief 解析 LLM 返回的原始 JSON，提取内层业务 JSON 或 fallback 文本
 * @param raw_response LLM API 原始返回
 * @param out_action 解析后的业务动作
 * @return 错误码
 */
llm_error_t parse_llm_response(const std::string& raw_response,
                               llm_action_t& out_action);
llm_error_t parse_llm_content(const std::string& content,
                              llm_action_t& out_action);

/**
 * @brief 执行 LLM 返回的业务动作，并投递 EVT_LLM_RESPONSE_READY
 * @param action 业务动作
 * @param out_reply 用于接收 TTS 文本
 * @return 错误码
 */
llm_error_t execute_llm_action(const llm_action_t& action,
                               std::string& out_reply,
                               bool tts_already_queued = false);

/**
 * @brief 处理“确认/取消”文本；若存在待确认库存操作则在本地完成，不调用云端模型
 */
pending_confirmation_result_t handle_pending_inventory_confirmation(
    const std::string& user_text, std::string& out_reply);

} // namespace ai
} // namespace smart_fridge
