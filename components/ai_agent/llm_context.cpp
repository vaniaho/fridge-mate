#include "llm_context.hpp"
#include "llm_config.hpp"
#include "inventory.hpp"
#include "inventory_history.hpp"
#include "recipe_matcher.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <sstream>
#include <cstdio>

static const char *TAG = "LLMContext";

namespace smart_fridge {
namespace ai {

namespace {

llm_context_cache_t s_cache;
std::vector<llm_conversation_message_t> s_conversation;
SemaphoreHandle_t s_conversation_mutex = nullptr;
constexpr size_t MAX_CONVERSATION_MESSAGES = 12;

SemaphoreHandle_t conversation_mutex() {
    if (!s_conversation_mutex) {
        s_conversation_mutex = xSemaphoreCreateMutex();
    }
    return s_conversation_mutex;
}

int64_t get_tick_ms() {
    return (int64_t)xTaskGetTickCount() * 1000 / configTICK_RATE_HZ;
}

std::string build_inventory_text() {
    auto inventory = smart_fridge::inventory::get_all_ingredients();
    if (inventory.empty()) {
        return "当前冰箱是空的。";
    }

    std::stringstream ss;
    ss << "当前冰箱库存有: ";
    int count = 0;
    for (const auto& item : inventory) {
        if (count >= config::AI_LLM_MAX_PROMPT_INVENTORY) {
            ss << "等" << (int)(inventory.size() - count) << "种食材, ";
            break;
        }
        ss << item.name << "(" << item.total_quantity << "个), ";
        count++;
    }
    return ss.str();
}

std::string build_rates_text() {
    auto rates = smart_fridge::inventory::compute_all_consumption_rates(7);
    if (rates.empty()) {
        return "";
    }

    std::stringstream ss;
    ss << "食材消耗统计(近7天): ";
    for (const auto& r : rates) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.1f", r.daily_rate);
        ss << r.item_name << "日均消耗" << buf << "个, ";
    }
    return ss.str();
}

std::string build_recipes_text() {
    auto recipe_matches = smart_fridge::inventory::recipe_match_near(2);
    if (recipe_matches.empty()) {
        return "";
    }

    std::stringstream ss;
    ss << "可做菜品推荐: ";
    int count = 0;
    for (const auto& m : recipe_matches) {
        if (count >= config::AI_LLM_MAX_PROMPT_RECIPES) break;
        ss << m.recipe.name;
        if (m.missing_count == 0) {
            ss << "(食材齐全)";
        } else {
            ss << "(差";
            for (size_t i = 0; i < m.missing_items.size(); i++) {
                if (i > 0) ss << "、";
                ss << m.missing_items[i];
            }
            ss << ")";
        }
        ss << ", ";
        count++;
    }
    return ss.str();
}

void rebuild_cache() {
    s_cache.inventory_text = build_inventory_text();
    s_cache.rates_text = build_rates_text();
    s_cache.recipes_text = build_recipes_text();
    s_cache.timestamp_ms = get_tick_ms();
    s_cache.valid = true;

    ESP_LOGI(TAG, "Context cache rebuilt");
}

} // anonymous namespace

const llm_context_cache_t& get_llm_context_cache(void) {
    int64_t now = get_tick_ms();
    if (!s_cache.valid ||
        (now - s_cache.timestamp_ms) > config::AI_CONTEXT_CACHE_TTL_MS) {
        rebuild_cache();
    }
    return s_cache;
}

void invalidate_llm_context_cache(void) {
    s_cache.valid = false;
    ESP_LOGI(TAG, "Context cache invalidated");
}

std::vector<llm_conversation_message_t> get_llm_conversation(void) {
    SemaphoreHandle_t mutex = conversation_mutex();
    if (!mutex) return {};
    xSemaphoreTake(mutex, portMAX_DELAY);
    auto copy = s_conversation;
    xSemaphoreGive(mutex);
    return copy;
}

void append_llm_conversation_turn(const std::string& user,
                                  const std::string& assistant) {
    if (user.empty() || assistant.empty()) return;
    SemaphoreHandle_t mutex = conversation_mutex();
    if (!mutex) return;
    xSemaphoreTake(mutex, portMAX_DELAY);
    s_conversation.push_back({"user", user});
    s_conversation.push_back({"assistant", assistant});
    while (s_conversation.size() > MAX_CONVERSATION_MESSAGES) {
        s_conversation.erase(s_conversation.begin(),
                             s_conversation.begin() + 2);
    }
    xSemaphoreGive(mutex);
}

void clear_llm_conversation(void) {
    SemaphoreHandle_t mutex = conversation_mutex();
    if (!mutex) return;
    xSemaphoreTake(mutex, portMAX_DELAY);
    s_conversation.clear();
    xSemaphoreGive(mutex);
}

} // namespace ai
} // namespace smart_fridge
