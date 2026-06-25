#include "llm_internal.hpp"
#include "llm_context.hpp"
#include "llm_utils.hpp"
#include "inventory.hpp"
#include "category_table.h"
#include "system_events.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <vector>

static const char *TAG = "LLMResponseExecutor";

namespace smart_fridge {
namespace ai {

namespace {

SemaphoreHandle_t s_pending_mutex = NULL;
llm_action_t s_pending_action;
bool s_has_pending_action = false;
TickType_t s_pending_created_tick = 0;
constexpr TickType_t PENDING_ACTION_TTL = pdMS_TO_TICKS(60000);

SemaphoreHandle_t pending_mutex() {
    if (!s_pending_mutex) {
        s_pending_mutex = xSemaphoreCreateMutex();
    }
    return s_pending_mutex;
}

std::string trim_text(const std::string& text) {
    size_t start = text.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = text.find_last_not_of(" \t\r\n");
    std::string normalized = text.substr(start, end - start + 1);
    const char* suffixes[] = {"。", "！", "？", "!", "?"};
    bool removed = true;
    while (removed && !normalized.empty()) {
        removed = false;
        for (const char* suffix : suffixes) {
            size_t suffix_len = strlen(suffix);
            if (normalized.size() >= suffix_len &&
                normalized.compare(normalized.size() - suffix_len, suffix_len, suffix) == 0) {
                normalized.erase(normalized.size() - suffix_len);
                removed = true;
                break;
            }
        }
    }
    return normalized;
}

std::string trim_whitespace(const std::string& text) {
    size_t start = text.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(start, end - start + 1);
}

bool is_confirmation_text(const std::string& text) {
    const std::string normalized = trim_text(text);
    return normalized == "确认" || normalized == "确定" ||
           normalized == "执行" || normalized == "确认执行";
}

bool is_cancellation_text(const std::string& text) {
    const std::string normalized = trim_text(text);
    return normalized == "取消" || normalized == "不要" ||
           normalized == "算了" || normalized == "否" ||
           normalized == "不执行";
}

void save_pending_action(const llm_action_t& action) {
    SemaphoreHandle_t mutex = pending_mutex();
    if (!mutex) return;
    xSemaphoreTake(mutex, portMAX_DELAY);
    s_pending_action = action;
    s_has_pending_action = true;
    s_pending_created_tick = xTaskGetTickCount();
    xSemaphoreGive(mutex);
}

bool take_pending_action(llm_action_t& action) {
    SemaphoreHandle_t mutex = pending_mutex();
    if (!mutex) return false;
    xSemaphoreTake(mutex, portMAX_DELAY);
    if (!s_has_pending_action) {
        xSemaphoreGive(mutex);
        return false;
    }
    if ((xTaskGetTickCount() - s_pending_created_tick) > PENDING_ACTION_TTL) {
        s_pending_action = {};
        s_has_pending_action = false;
        s_pending_created_tick = 0;
        xSemaphoreGive(mutex);
        return false;
    }
    action = s_pending_action;
    s_pending_action = {};
    s_has_pending_action = false;
    s_pending_created_tick = 0;
    xSemaphoreGive(mutex);
    return true;
}

bool clear_pending_action() {
    SemaphoreHandle_t mutex = pending_mutex();
    if (!mutex) return false;
    xSemaphoreTake(mutex, portMAX_DELAY);
    bool existed = s_has_pending_action;
    s_pending_action = {};
    s_has_pending_action = false;
    s_pending_created_tick = 0;
    xSemaphoreGive(mutex);
    return existed;
}

std::string pending_prompt(const llm_action_t& action) {
    if (action.type == llm_action_type_t::BATCH) {
        std::string operations;
        for (const auto& item : action.actions) {
            if (!operations.empty()) operations += "、";
            operations += item.type == llm_action_type_t::ADD
                ? "存入" : "取出";
            operations += std::to_string(item.quantity) + "个" +
                          item.target_item;
        }
        return "准备" + operations +
               "，请说“确认”执行全部操作，或说“取消”放弃。";
    }
    char buf[256];
    if (action.type == llm_action_type_t::ADD) {
        snprintf(buf, sizeof(buf), "准备存入 %d 个%s，请说“确认”执行，或说“取消”放弃。",
                 action.quantity, action.target_item.c_str());
    } else {
        snprintf(buf, sizeof(buf), "准备取出 %d 个%s，请说“确认”执行，或说“取消”放弃。",
                 action.quantity, action.target_item.c_str());
    }
    return buf;
}

void normalize_inventory_action(llm_action_t& action) {
    action.target_item = trim_whitespace(action.target_item);
    action.category = trim_whitespace(action.category);
    if (action.type == llm_action_type_t::ADD) {
        const char* fallback_category =
            category_lookup(action.target_item.c_str());
        if (action.category.empty()) {
            action.category =
                fallback_category ? fallback_category : "其他";
        }
        if (action.expire_days <= 0) {
            action.expire_days =
                expire_days_for_item(action.target_item.c_str());
        }
    }
}

void normalize_batch(llm_action_t& batch) {
    std::vector<llm_action_t> merged;
    for (auto item : batch.actions) {
        normalize_inventory_action(item);
        auto existing = std::find_if(
            merged.begin(), merged.end(),
            [&](const llm_action_t& value) {
                return value.type == item.type &&
                       value.target_item == item.target_item;
            });
        if (existing == merged.end()) {
            merged.push_back(std::move(item));
        } else {
            existing->quantity += item.quantity;
        }
    }
    batch.actions = std::move(merged);
}

std::string inventory_failure_reply(
    const smart_fridge::inventory::InventoryResult& result) {
    std::string reply = smart_fridge::inventory::inventory_error_message(result.error);
    if (result.error == smart_fridge::inventory::InventoryError::INSUFFICIENT_QUANTITY) {
        reply += "，当前仅有 " + std::to_string(result.remaining_quantity) + " 个";
    }
    return reply;
}

smart_fridge::inventory::InventoryResult validate_pending_action(
    const llm_action_t& action) {
    if (action.type == llm_action_type_t::ADD) {
        return smart_fridge::inventory::validate_add_ingredient(
            action.target_item, action.category, action.quantity, action.expire_days);
    }
    return smart_fridge::inventory::validate_remove_ingredient(
        action.target_item, action.quantity);
}

smart_fridge::inventory::InventoryResult validate_pending_batch(
    const llm_action_t& batch) {
    for (const auto& action : batch.actions) {
        auto result = validate_pending_action(action);
        if (!result.ok()) return result;
    }
    return {};
}

smart_fridge::inventory::InventoryResult execute_confirmed_action(
    const llm_action_t& action) {
    if (action.type == llm_action_type_t::ADD) {
        return smart_fridge::inventory::add_ingredient_checked(
            action.target_item, action.category, action.quantity, action.expire_days);
    }
    return smart_fridge::inventory::remove_ingredient_checked(
        action.target_item, action.quantity);
}

} // anonymous namespace

llm_error_t execute_llm_action(const llm_action_t& action,
                               std::string& out_reply,
                               bool tts_already_queued) {
    out_reply = action.tts_reply.empty() ? "好的" : action.tts_reply;
    int ui_action_id = UI_ACTION_NONE;

    switch (action.type) {
        case llm_action_type_t::ADD: {
            if (action.target_item.empty() || action.quantity <= 0) {
                ESP_LOGE(TAG, "Invalid ADD action: missing target or quantity");
                out_reply = "存入指令参数不完整，请再说一次";
                dispatch_llm_response_event(out_reply, UI_ACTION_NONE,
                                            tts_already_queued);
                return llm_error_t::OK;
            }

            llm_action_t pending = action;
            normalize_inventory_action(pending);

            auto validation = validate_pending_action(pending);
            if (!validation.ok()) {
                out_reply = inventory_failure_reply(validation);
                dispatch_llm_response_event(out_reply, UI_ACTION_NONE,
                                            tts_already_queued);
                return llm_error_t::OK;
            }

            save_pending_action(pending);
            out_reply = pending_prompt(pending);
            dispatch_llm_response_event(out_reply, UI_ACTION_NONE,
                                        tts_already_queued);
            ESP_LOGI(TAG, "[LLM Action] ADD awaiting confirmation: %s x%d",
                     pending.target_item.c_str(), pending.quantity);
            return llm_error_t::OK;
        }

        case llm_action_type_t::REMOVE: {
            if (action.target_item.empty() || action.quantity <= 0) {
                ESP_LOGE(TAG, "Invalid REMOVE action: missing target or quantity");
                out_reply = "取出指令参数不完整，请再说一次";
                dispatch_llm_response_event(out_reply, UI_ACTION_NONE,
                                            tts_already_queued);
                return llm_error_t::OK;
            }

            llm_action_t pending = action;
            normalize_inventory_action(pending);
            auto validation = validate_pending_action(pending);
            if (!validation.ok()) {
                out_reply = inventory_failure_reply(validation);
                dispatch_llm_response_event(out_reply, UI_ACTION_NONE,
                                            tts_already_queued);
                return llm_error_t::OK;
            }

            save_pending_action(pending);
            out_reply = pending_prompt(pending);
            dispatch_llm_response_event(out_reply, UI_ACTION_NONE,
                                        tts_already_queued);
            ESP_LOGI(TAG, "[LLM Action] REMOVE awaiting confirmation: %s x%d",
                     pending.target_item.c_str(), pending.quantity);
            return llm_error_t::OK;
        }

        case llm_action_type_t::BATCH: {
            llm_action_t pending = action;
            normalize_batch(pending);
            if (pending.actions.empty()) {
                out_reply = "库存指令中没有可执行的操作，请再说一次";
                dispatch_llm_response_event(out_reply, UI_ACTION_NONE,
                                            tts_already_queued);
                return llm_error_t::OK;
            }
            for (const auto& item : pending.actions) {
                if ((item.type != llm_action_type_t::ADD &&
                     item.type != llm_action_type_t::REMOVE) ||
                    item.target_item.empty() || item.quantity <= 0) {
                    out_reply = "批量库存指令参数不完整，请再说一次";
                    dispatch_llm_response_event(out_reply, UI_ACTION_NONE,
                                                tts_already_queued);
                    return llm_error_t::OK;
                }
            }
            auto validation = validate_pending_batch(pending);
            if (!validation.ok()) {
                out_reply = inventory_failure_reply(validation);
                dispatch_llm_response_event(out_reply, UI_ACTION_NONE,
                                            tts_already_queued);
                return llm_error_t::OK;
            }
            save_pending_action(pending);
            out_reply = pending_prompt(pending);
            dispatch_llm_response_event(out_reply, UI_ACTION_NONE,
                                        tts_already_queued);
            ESP_LOGI(TAG, "[LLM Action] BATCH awaiting confirmation: %u actions",
                     static_cast<unsigned>(pending.actions.size()));
            return llm_error_t::OK;
        }

        case llm_action_type_t::RECIPE: {
            ESP_LOGI(TAG, "[LLM Action] Recipe recommendation");
            ui_action_id = UI_ACTION_SHOW_RECIPE;
            break;
        }

        case llm_action_type_t::CHAT:
        case llm_action_type_t::UNKNOWN:
        default:
            // 无需特殊业务操作，仅播报
            break;
    }

    dispatch_llm_response_event(out_reply, ui_action_id, tts_already_queued);
    return llm_error_t::OK;
}

pending_confirmation_result_t handle_pending_inventory_confirmation(
    const std::string& user_text, std::string& out_reply) {
    bool confirm = is_confirmation_text(user_text);
    bool cancel = is_cancellation_text(user_text);
    if (!confirm && !cancel) {
        return pending_confirmation_result_t::NOT_HANDLED;
    }

    if (cancel) {
        if (clear_pending_action()) {
            out_reply = "已取消本次库存操作";
        } else {
            out_reply = "当前没有待取消的库存操作";
        }
        dispatch_llm_response_event(out_reply, UI_ACTION_NONE);
        return pending_confirmation_result_t::HANDLED;
    }

    llm_action_t pending;
    if (!take_pending_action(pending)) {
        out_reply = "当前没有待确认的库存操作";
        dispatch_llm_response_event(out_reply, UI_ACTION_NONE);
        return pending_confirmation_result_t::HANDLED;
    }

    if (pending.type == llm_action_type_t::BATCH) {
        auto validation = validate_pending_batch(pending);
        if (!validation.ok()) {
            out_reply = inventory_failure_reply(validation);
            dispatch_llm_response_event(out_reply, UI_ACTION_NONE);
            return pending_confirmation_result_t::HANDLED;
        }

        std::string completed;
        for (const auto& action : pending.actions) {
            auto result = execute_confirmed_action(action);
            if (!result.ok()) {
                out_reply = completed.empty()
                    ? inventory_failure_reply(result)
                    : completed + "；后续操作失败：" +
                      inventory_failure_reply(result);
                invalidate_llm_context_cache();
                send_system_event(EVT_INVENTORY_UPDATED, NULL, 0);
                dispatch_llm_response_event(out_reply,
                                            UI_ACTION_REFRESH_LIST);
                return pending_confirmation_result_t::HANDLED;
            }
            if (!completed.empty()) completed += "，";
            completed += action.type == llm_action_type_t::ADD
                ? "已存入" : "已取出";
            completed += std::to_string(result.affected_quantity) +
                         "个" + action.target_item;
        }
        invalidate_llm_context_cache();
        send_system_event(EVT_INVENTORY_UPDATED, NULL, 0);
        out_reply = completed;
        dispatch_llm_response_event(out_reply, UI_ACTION_REFRESH_LIST);
        return pending_confirmation_result_t::HANDLED;
    }

    auto result = execute_confirmed_action(pending);
    if (!result.ok()) {
        out_reply = inventory_failure_reply(result);
        dispatch_llm_response_event(out_reply, UI_ACTION_NONE);
        return pending_confirmation_result_t::HANDLED;
    }
    invalidate_llm_context_cache();
    send_system_event(EVT_INVENTORY_UPDATED, NULL, 0);

    if (pending.type == llm_action_type_t::ADD) {
        out_reply = "已存入 " + std::to_string(result.affected_quantity) +
                    " 个" + pending.target_item;
    } else {
        out_reply = "已取出 " + std::to_string(result.affected_quantity) +
                    " 个" + pending.target_item;
    }
    dispatch_llm_response_event(out_reply, UI_ACTION_REFRESH_LIST);
    return pending_confirmation_result_t::HANDLED;
}

} // namespace ai
} // namespace smart_fridge
