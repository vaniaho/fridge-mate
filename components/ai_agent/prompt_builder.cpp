#include "ai_agent.hpp"
#include "inventory.hpp"
#include "cJSON.h"
#include "esp_log.h"
#include "credentials_manager.h"
#include "system_events.h"
#include <sstream>
#include <cstring>
#include <cstdlib>

static const char *TAG = "PromptBuilder";

namespace smart_fridge {
namespace ai {

std::string build_llm_request(const std::string& user_voice_text) {
    // 1. 抓取当前冰箱库存
    auto inventory = smart_fridge::inventory::get_all_ingredients();
    std::stringstream inventory_context;
    
    if (inventory.empty()) {
        inventory_context << "当前冰箱是空的。";
    } else {
        inventory_context << "当前冰箱库存有: ";
        for (const auto& item : inventory) {
            inventory_context << item.name << "(" << item.quantity << "), ";
        }
    }

    // 2. 抓取消耗速率数据（过去 7 天）
    auto rates = smart_fridge::inventory::compute_all_consumption_rates(7);
    std::stringstream rate_context;
    if (!rates.empty()) {
        rate_context << "食材消耗统计(近7天): ";
        for (const auto& r : rates) {
            char buf[64];
            snprintf(buf, sizeof(buf), "%.1f", r.daily_rate);
            rate_context << r.item_name << "日均消耗" << buf << "个, ";
        }
    }

    // 3. 抓取食谱推荐（差2样以内的菜品）
    auto recipe_matches = smart_fridge::inventory::recipe_match_near(2);
    std::stringstream recipe_context;
    if (!recipe_matches.empty()) {
        recipe_context << "可做菜品推荐: ";
        int count = 0;
        for (const auto& m : recipe_matches) {
            if (count >= 5) break; // 最多注入5个推荐，避免 Prompt 过长
            recipe_context << m.recipe.name;
            if (m.missing_count == 0) {
                recipe_context << "(食材齐全)";
            } else {
                recipe_context << "(差";
                for (size_t i = 0; i < m.missing_items.size(); i++) {
                    if (i > 0) recipe_context << "、";
                    recipe_context << m.missing_items[i];
                }
                recipe_context << ")";
            }
            recipe_context << ", ";
            count++;
        }
    }

    // 4. 构建 System Prompt
    std::stringstream system_prompt;
    system_prompt << "你是一个名为\"小鲜\"的智能冰箱助手。你的任务是根据用户的语音指令和当前库存状态，返回对应的JSON操作指令和语音回复。\n";
    system_prompt << inventory_context.str() << "\n";
    if (!rates.empty()) {
        system_prompt << rate_context.str() << "\n";
    }
    if (!recipe_matches.empty()) {
        system_prompt << recipe_context.str() << "\n";
    }
    system_prompt << R"PROMPT(
必须严格返回以下JSON格式，不要包含任何markdown标记或其他文字：
{
    "action": "ADD" | "REMOVE" | "RECIPE" | "CHAT",
    "target_item": "食材名称(如苹果, 仅在ADD/REMOVE时需要)",
    "category": "食材分类(如水果/蕴菜/肉禽/海鲜/蛋奶/豆制品/主食/调味品/饮品/零食/冻品, 仅在ADD时需要)",
    "expire_days": 保质期天数(整数, 仅在ADD时需要, 根据食材类型合理估算),
    "quantity": 数量(整数, 仅在ADD/REMOVE时需要),
    "tts_reply": "你应该回答给用户的语音播报内容"
}
当用户问"今天吃什么"或"推荐菜品"等问题时，请结合上面的可做菜品推荐来回答，action设为"RECIPE"。
)PROMPT";

    // 3. 构建发给 LLM API 的完整 JSON Body (以 OpenAI/GLM 格式为例)
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", credentials_get_llm_model());

    cJSON *messages = cJSON_CreateArray();
    
    // System message
    cJSON *sys_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(sys_msg, "role", "system");
    cJSON_AddStringToObject(sys_msg, "content", system_prompt.str().c_str());
    cJSON_AddItemToArray(messages, sys_msg);

    // User message
    cJSON *usr_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(usr_msg, "role", "user");
    cJSON_AddStringToObject(usr_msg, "content", user_voice_text.c_str());
    cJSON_AddItemToArray(messages, usr_msg);

    cJSON_AddItemToObject(root, "messages", messages);

    char *json_string = cJSON_PrintUnformatted(root);
    std::string result(json_string);
    
    free(json_string);
    cJSON_Delete(root);

    return result;
}

std::string parse_and_execute_llm_response(const std::string& llm_json_response) {
    ESP_LOGI(TAG, "Parsing LLM response...");
    
    cJSON *root = cJSON_Parse(llm_json_response.c_str());
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse outer JSON");
        return "";
    }

    // OpenAI 格式返回的是 choices[0].message.content
    cJSON *choices = cJSON_GetObjectItem(root, "choices");
    if (choices && cJSON_GetArraySize(choices) > 0) {
        cJSON *first_choice = cJSON_GetArrayItem(choices, 0);
        cJSON *message = cJSON_GetObjectItem(first_choice, "message");
        cJSON *content = cJSON_GetObjectItem(message, "content");

        if (content && content->valuestring) {
            std::string inner_json_str = content->valuestring;
            // 尝试解析我们要求模型返回的内层业务 JSON
            cJSON *inner_json = cJSON_Parse(inner_json_str.c_str());
            if (inner_json) {
                cJSON *action = cJSON_GetObjectItem(inner_json, "action");
                cJSON *target_item = cJSON_GetObjectItem(inner_json, "target_item");
                cJSON *quantity = cJSON_GetObjectItem(inner_json, "quantity");
                cJSON *tts_reply = cJSON_GetObjectItem(inner_json, "tts_reply");

                if (action && action->valuestring) {
                    std::string act_str = action->valuestring;
                    std::string tts_str = tts_reply ? tts_reply->valuestring : "好的";
                    int ui_action = UI_ACTION_NONE;
                    
                    if (act_str == "ADD" && target_item && quantity) {
                        // 优先使用 LLM 返回的 category，否则用本地品类表查询
                        cJSON *cat_json = cJSON_GetObjectItem(inner_json, "category");
                        cJSON *exp_json = cJSON_GetObjectItem(inner_json, "expire_days");
                        
                        const char* item_name = target_item->valuestring;
                        const char* cat = (cat_json && cat_json->valuestring && strlen(cat_json->valuestring) > 0)
                                          ? cat_json->valuestring
                                          : category_lookup(item_name);
                        int exp_days = (exp_json && exp_json->valueint > 0)
                                       ? exp_json->valueint
                                       : expire_days_for_item(item_name);
                        
                        smart_fridge::inventory::add_ingredient(item_name, cat, quantity->valueint, exp_days);
                        ESP_LOGI(TAG, "[LLM Action] ADD: %s x%d, category=%s, expire=%d days",
                                 item_name, quantity->valueint, cat, exp_days);
                        ui_action = UI_ACTION_REFRESH_LIST;
                        
                        // 投递库存更新事件，通知 GUI 刷新
                        send_system_event(EVT_INVENTORY_UPDATED, NULL, 0);
                    } 
                    else if (act_str == "REMOVE" && target_item && quantity) {
                        smart_fridge::inventory::remove_ingredient(target_item->valuestring, quantity->valueint);
                        ESP_LOGI(TAG, "[LLM Action] REMOVE: %s x%d", target_item->valuestring, quantity->valueint);
                        ui_action = UI_ACTION_REFRESH_LIST;
                        
                        // 投递库存更新事件
                        send_system_event(EVT_INVENTORY_UPDATED, NULL, 0);
                    }
                    else if (act_str == "RECIPE") {
                        ESP_LOGI(TAG, "[LLM Action] Recipe recommendation.");
                        ui_action = UI_ACTION_SHOW_RECIPE;
                    }
                    // CHAT: 无特殊业务操作，只播报 tts_reply
                    
                    // 封装 LLM 响应 payload，投递到事件总线
                    // B 模块收到后播报 TTS，A 模块收到后更新 GUI
                    llm_response_payload_t* payload = (llm_response_payload_t*)malloc(sizeof(llm_response_payload_t));
                    if (payload) {
                        memset(payload, 0, sizeof(llm_response_payload_t));
                        strncpy(payload->tts_text, tts_str.c_str(), sizeof(payload->tts_text) - 1);
                        payload->ui_action_id = ui_action;
                        
                        esp_err_t err = send_system_event(EVT_LLM_RESPONSE_READY, payload, sizeof(llm_response_payload_t));
                        if (err != ESP_OK) {
                            ESP_LOGE(TAG, "Failed to send EVT_LLM_RESPONSE_READY");
                            free(payload);
                        } else {
                            ESP_LOGI(TAG, "[Event] EVT_LLM_RESPONSE_READY dispatched. TTS: %s", tts_str.c_str());
                        }
                    }
                    cJSON_Delete(inner_json);
                    cJSON_Delete(root);
                    return tts_str;
                }
                cJSON_Delete(inner_json);
            } else {
                ESP_LOGE(TAG, "Inner content is not valid JSON: %s", inner_json_str.c_str());
            }
        }
    }

    cJSON_Delete(root);
    return "";
}

} // namespace ai
} // namespace smart_fridge
