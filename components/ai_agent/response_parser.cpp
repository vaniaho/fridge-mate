#include "llm_internal.hpp"
#include "cJSON.h"
#include "esp_log.h"
#include <cstring>
#include <algorithm>

static const char *TAG = "LLMResponseParser";

namespace smart_fridge {
namespace ai {

namespace {

// 去除首尾空白
std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

// 去除 markdown 代码块标记，提取内部 JSON
std::string strip_markdown_code_block(const std::string& s) {
    if (s.length() >= 6 && s.substr(0, 3) == "```") {
        size_t first_nl = s.find('\n');
        if (first_nl == std::string::npos) return s;
        size_t end_block = s.rfind("```");
        if (end_block == std::string::npos || end_block <= first_nl) return s;
        return trim(s.substr(first_nl + 1, end_block - first_nl - 1));
    }
    return s;
}

// 将字符串中的 action 转为枚举
llm_action_type_t parse_action_type(const char* action_str) {
    if (!action_str) return llm_action_type_t::UNKNOWN;
    if (strcmp(action_str, "ADD") == 0) return llm_action_type_t::ADD;
    if (strcmp(action_str, "REMOVE") == 0) return llm_action_type_t::REMOVE;
    if (strcmp(action_str, "BATCH") == 0) return llm_action_type_t::BATCH;
    if (strcmp(action_str, "RECIPE") == 0) return llm_action_type_t::RECIPE;
    if (strcmp(action_str, "CHAT") == 0) return llm_action_type_t::CHAT;
    return llm_action_type_t::UNKNOWN;
}

bool parse_action_object(cJSON* object, llm_action_t& action) {
    if (!cJSON_IsObject(object)) return false;
    cJSON* action_json = cJSON_GetObjectItem(object, "action");
    if (!cJSON_IsString(action_json) || !action_json->valuestring) {
        return false;
    }

    action = {};
    action.type = parse_action_type(action_json->valuestring);
    if (action.type == llm_action_type_t::UNKNOWN) return false;

    cJSON* target_item = cJSON_GetObjectItem(object, "target_item");
    cJSON* category = cJSON_GetObjectItem(object, "category");
    cJSON* expire_days = cJSON_GetObjectItem(object, "expire_days");
    cJSON* quantity = cJSON_GetObjectItem(object, "quantity");
    cJSON* tts_reply = cJSON_GetObjectItem(object, "tts_reply");

    if (cJSON_IsString(target_item) && target_item->valuestring) {
        action.target_item = trim(target_item->valuestring);
    }
    if (cJSON_IsString(category) && category->valuestring) {
        action.category = trim(category->valuestring);
    }
    if (cJSON_IsNumber(expire_days)) {
        action.expire_days = expire_days->valueint;
    }
    if (cJSON_IsNumber(quantity)) {
        action.quantity = quantity->valueint;
    }
    if (cJSON_IsString(tts_reply) && tts_reply->valuestring) {
        action.tts_reply = trim(tts_reply->valuestring);
    }

    if (action.type == llm_action_type_t::BATCH) {
        cJSON* actions = cJSON_GetObjectItem(object, "actions");
        if (!cJSON_IsArray(actions)) return false;
        cJSON* child = nullptr;
        cJSON_ArrayForEach(child, actions) {
            llm_action_t parsed;
            if (!parse_action_object(child, parsed) ||
                (parsed.type != llm_action_type_t::ADD &&
                 parsed.type != llm_action_type_t::REMOVE)) {
                return false;
            }
            action.actions.push_back(std::move(parsed));
        }
        return !action.actions.empty();
    }

    if (action.tts_reply.empty()) action.tts_reply = "好的";
    return true;
}

// 从外层 OpenAI 格式中提取 content 字符串
llm_error_t extract_content_string(const std::string& raw_response, std::string& out_content) {
    cJSON* root = cJSON_Parse(raw_response.c_str());
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse outer JSON");
        return llm_error_t::ERR_JSON_INVALID;
    }

    llm_error_t result = llm_error_t::ERR_LLM_REFUSAL;

    cJSON* choices = cJSON_GetObjectItem(root, "choices");
    if (choices && cJSON_IsArray(choices) && cJSON_GetArraySize(choices) > 0) {
        cJSON* first_choice = cJSON_GetArrayItem(choices, 0);
        cJSON* message = cJSON_GetObjectItem(first_choice, "message");
        if (message) {
            cJSON* content = cJSON_GetObjectItem(message, "content");
            if (content && cJSON_IsString(content) && content->valuestring) {
                out_content = content->valuestring;
                result = llm_error_t::OK;
            }
        }
    }

    // fallback: 某些服务可能直接返回 { "content": "..." }
    if (result != llm_error_t::OK) {
        cJSON* content = cJSON_GetObjectItem(root, "content");
        if (content && cJSON_IsString(content) && content->valuestring) {
            out_content = content->valuestring;
            result = llm_error_t::OK;
        }
    }

    cJSON_Delete(root);
    return result;
}

} // anonymous namespace

llm_error_t parse_llm_response(const std::string& raw_response,
                               llm_action_t& out_action) {
    std::string content;
    llm_error_t err = extract_content_string(raw_response, content);
    if (err != llm_error_t::OK) {
        return err;
    }
    return parse_llm_content(content, out_action);
}

llm_error_t parse_llm_content(const std::string& raw_content,
                              llm_action_t& out_action) {
    out_action = {};
    std::string content = raw_content;
    content = trim(content);
    if (content.empty()) {
        return llm_error_t::ERR_LLM_REFUSAL;
    }

    // 尝试解析内层业务 JSON
    std::string inner_json_str = strip_markdown_code_block(content);
    cJSON* inner_json = cJSON_Parse(inner_json_str.c_str());

    if (inner_json) {
        bool parsed = false;
        if (cJSON_IsArray(inner_json)) {
            out_action.type = llm_action_type_t::BATCH;
            cJSON* child = nullptr;
            cJSON_ArrayForEach(child, inner_json) {
                llm_action_t item;
                if (!parse_action_object(child, item) ||
                    (item.type != llm_action_type_t::ADD &&
                     item.type != llm_action_type_t::REMOVE)) {
                    parsed = false;
                    out_action = {};
                    break;
                }
                out_action.actions.push_back(std::move(item));
                parsed = true;
            }
        } else {
            parsed = parse_action_object(inner_json, out_action);
        }
        cJSON_Delete(inner_json);
        if (parsed) return llm_error_t::OK;
        ESP_LOGE(TAG, "Business JSON has an invalid action shape");
        return llm_error_t::ERR_ACTION_INVALID;
    }

    // Fallback: 内层不是 JSON，当作普通 CHAT 处理
    ESP_LOGW(TAG, "Inner content is not valid JSON, fallback to CHAT");
    out_action.type = llm_action_type_t::CHAT;
    out_action.tts_reply = content;
    return llm_error_t::OK;
}

} // namespace ai
} // namespace smart_fridge
