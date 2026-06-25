#pragma once

#include <string>
#include <cstdint>
#include <vector>

namespace smart_fridge {
namespace ai {

/**
 * @brief 库存上下文缓存
 * 用于减少每次 LLM 请求时反复遍历库存、历史、食谱的开销。
 */
struct llm_context_cache_t {
    std::string inventory_text;
    std::string rates_text;
    std::string recipes_text;
    int64_t timestamp_ms = 0;
    bool valid = false;
};

struct llm_conversation_message_t {
    std::string role;
    std::string content;
};

/**
 * @brief 获取当前库存上下文（带 TTL 缓存）
 * @return 缓存结构引用
 */
const llm_context_cache_t& get_llm_context_cache(void);

/**
 * @brief 强制刷新库存上下文缓存
 */
void invalidate_llm_context_cache(void);
std::vector<llm_conversation_message_t> get_llm_conversation(void);
void append_llm_conversation_turn(const std::string& user,
                                  const std::string& assistant);
void clear_llm_conversation(void);

} // namespace ai
} // namespace smart_fridge
