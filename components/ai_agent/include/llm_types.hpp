#pragma once

#include <string>
#include <cstdint>
#include <vector>

namespace smart_fridge {
namespace ai {

/**
 * @brief LLM 调用错误码
 */
enum class llm_error_t {
    OK = 0,
    ERR_UNKNOWN,
    ERR_NO_API_KEY,       // API Key 未配置
    ERR_MEMORY,           // 内存分配失败
    ERR_NETWORK,          // 通用网络错误
    ERR_DNS,              // DNS 解析失败
    ERR_TLS,              // TLS/证书错误
    ERR_HTTP_4XX,         // 客户端错误（Key/模型/参数问题）
    ERR_HTTP_5XX,         // 服务端错误
    ERR_TIMEOUT,          // 请求/响应超时
    ERR_JSON_INVALID,     // 返回内容不是合法 JSON
    ERR_LLM_REFUSAL,      // 模型拒绝回答或返回空 choices
    ERR_ACTION_INVALID,   // 模型返回的 action 无法识别
    ERR_QUEUE_FULL,       // AI Worker 队列已满
};

/**
 * @brief LLM 返回的业务动作类型
 */
enum class llm_action_type_t {
    CHAT,
    ADD,
    REMOVE,
    BATCH,
    RECIPE,
    UNKNOWN
};

/**
 * @brief 解析后的 LLM 业务动作
 */
struct llm_action_t {
    llm_action_type_t type = llm_action_type_t::UNKNOWN;
    std::string target_item;
    std::string category;
    int quantity = 0;
    int expire_days = 0;
    std::string unit;
    std::string tts_reply;
    std::vector<llm_action_t> actions;
};

/**
 * @brief LLM 调用统计
 */
struct llm_stats_t {
    uint32_t total_calls = 0;
    uint32_t failed_calls = 0;
    uint32_t last_latency_ms = 0;
    llm_error_t last_error = llm_error_t::OK;
};

/**
 * @brief 判断错误是否可重试
 */
inline bool is_llm_error_retriable(llm_error_t err) {
    return err == llm_error_t::ERR_TIMEOUT ||
           err == llm_error_t::ERR_NETWORK ||
           err == llm_error_t::ERR_DNS ||
           err == llm_error_t::ERR_HTTP_5XX;
}

/**
 * @brief 错误码转用户友好提示
 */
inline const char* llm_error_to_user_hint(llm_error_t err) {
    switch (err) {
        case llm_error_t::OK: return "好的";
        case llm_error_t::ERR_NO_API_KEY: return "请先配置大模型 API Key";
        case llm_error_t::ERR_MEMORY: return "内存不足，请稍后再试";
        case llm_error_t::ERR_NETWORK: return "网络连接异常，请检查网络";
        case llm_error_t::ERR_DNS: return "无法解析大模型服务地址";
        case llm_error_t::ERR_TLS: return "HTTPS 证书验证失败";
        case llm_error_t::ERR_HTTP_4XX: return "大模型接口参数错误，请检查 Key 和模型名";
        case llm_error_t::ERR_HTTP_5XX: return "大模型服务繁忙，请稍后再试";
        case llm_error_t::ERR_TIMEOUT: return "大模型响应超时，请重试";
        case llm_error_t::ERR_JSON_INVALID: return "大模型返回格式异常";
        case llm_error_t::ERR_LLM_REFUSAL: return "大模型未返回有效结果";
        case llm_error_t::ERR_ACTION_INVALID: return "无法识别该指令";
        case llm_error_t::ERR_QUEUE_FULL: return "大模型请求队列已满，请稍后再试";
        default: return "大模型调用失败";
    }
}

} // namespace ai
} // namespace smart_fridge
