#pragma once

#include <string>
#include "llm_types.hpp"

namespace smart_fridge {
namespace ai {

    /**
     * @brief 根据用户输入文本和当前库存上下文，组装 LLM 请求体（OpenAI 兼容格式）
     */
    std::string build_llm_request(const std::string& user_text,
                                  bool stream = false);

    /**
     * @brief 执行一次完整的 LLM 调用：HTTP 请求 + 解析 + 执行动作
     * @param user_text 用户输入文本
     * @param out_reply 用于接收模型回复文本（tts_reply）
     * @return 详细错误码
     */
    llm_error_t call_llm_api(const std::string& user_text, std::string& out_reply);

    /**
     * @brief 异步调用 LLM，不阻塞调用者
     * @param user_text 用户输入文本
     * @return true 成功入队
     */
    bool call_llm_api_async(const std::string& user_text);

    /**
     * Cancel the active streamed request and discard queued stale requests.
     * Used by wake-word/touch barge-in.
     */
    void cancel_llm_api(void);

    /**
     * @brief 获取 LLM 调用统计（可用于 Web 面板展示）
     */
    llm_stats_t get_llm_stats(void);

} // namespace ai
} // namespace smart_fridge
