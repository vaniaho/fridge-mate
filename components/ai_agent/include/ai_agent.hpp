#pragma once

#include <string>

namespace smart_fridge {
namespace ai {

    /**
     * @brief 根据本地食材库存和用户的语音命令，组装发给大模型的完整 JSON 请求串
     */
    std::string build_llm_request(const std::string& user_voice_text);

    /**
     * @brief 将大模型返回的 JSON 解析并执行对应的动作（如存入冰箱、投递事件给任务管理器等）
     * @return 返回大模型的回复文本（tts_reply）
     */
    std::string parse_and_execute_llm_response(const std::string& llm_json_response);

    /**
     * @brief 阻塞式调用大模型 API 并处理结果
     * @param user_voice_text 用户转换出的语音文本指令，如"帮我存入两个苹果"
     * @param out_reply 用于接收大模型的回复文本
     * @return true 调用成功, false 失败
     */
    bool call_llm_api(const std::string& user_voice_text, std::string& out_reply);

    /**
     * @brief 异步调用大模型 API
     * 将请求放入后台 worker 队列，由独立任务执行 HTTP 请求，不阻塞事件总线。
     * @param user_voice_text 用户转换出的语音文本指令
     * @return true 成功入队, false 失败
     */
    bool call_llm_api_async(const std::string& user_voice_text);

} // namespace ai
} // namespace smart_fridge
