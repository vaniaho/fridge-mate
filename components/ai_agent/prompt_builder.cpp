#include "ai_agent.hpp"
#include "llm_context.hpp"
#include "llm_config.hpp"
#include "credentials_manager.h"
#include "cJSON.h"
#include "esp_log.h"
#include <sstream>
#include <cstring>

static const char *TAG = "PromptBuilder";

namespace smart_fridge {
namespace ai {

namespace {

std::string build_system_prompt(const llm_context_cache_t& ctx) {
    std::stringstream ss;
    ss << "你是一个名为\"小鲜\"的智能冰箱助手。你的任务是根据用户的语音指令和当前库存状态，";
    ss << "返回对应的JSON操作指令和语音回复。\n";
    ss << ctx.inventory_text << "\n";
    if (!ctx.rates_text.empty()) {
        ss << ctx.rates_text << "\n";
    }
    if (!ctx.recipes_text.empty()) {
        ss << ctx.recipes_text << "\n";
    }
    ss << "必须严格返回以下JSON格式，不要包含任何markdown标记或其他文字：\n";
    ss << "字段必须严格按下面顺序输出，action 必须在 tts_reply 之前：\n";
    ss << "{\n";
    ss << "    \"action\": \"ADD\" | \"REMOVE\" | \"BATCH\" | \"RECIPE\" | \"CHAT\",\n";
    ss << "    \"tts_reply\": \"你应该回答给用户的语音播报内容\",\n";
    ss << "    \"target_item\": \"食材名称(如苹果, 仅在ADD/REMOVE时使用)\",\n";
    ss << "    \"category\": \"食材分类(如水果/蔬菜/肉禽/海鲜/蛋奶/豆制品/主食/调味品/饮品/零食/冻品, 仅在ADD时使用)\",\n";
    ss << "    \"expire_days\": 保质期天数(整数, 仅在ADD时使用, 根据食材类型合理估算),\n";
    ss << "    \"quantity\": 数量(整数, 仅在ADD/REMOVE时使用)\n";
    ss << "}\n";
    ss << "当一句话包含多个存入/取出操作时，禁止返回顶层JSON数组，必须返回：\n";
    ss << "{\"action\":\"BATCH\",\"tts_reply\":\"简洁确认提示\",\"actions\":[";
    ss << "{\"action\":\"ADD或REMOVE\",\"target_item\":\"食材\",\"category\":\"分类\",\"expire_days\":天数,\"quantity\":数量}";
    ss << "]}\n";
    ss << "BATCH.actions 中只允许 ADD 或 REMOVE，每种食材一个动作。\n";
    ss << "当用户问\"今天吃什么\"或\"推荐菜品\"等问题时，请结合上面的可做菜品推荐来回答，action设为\"RECIPE\"。\n";
    return ss.str();
}

} // anonymous namespace

std::string build_llm_request(const std::string& user_text, bool stream) {
    const auto& ctx = get_llm_context_cache();
    std::string system_prompt = build_system_prompt(ctx);

    cJSON *root = cJSON_CreateObject();
    if (!root) return "";

    cJSON_AddStringToObject(root, "model", credentials_get_llm_model());
    cJSON_AddBoolToObject(root, "stream", stream);

    cJSON *messages = cJSON_CreateArray();
    if (!messages) {
        cJSON_Delete(root);
        return "";
    }

    cJSON *sys_msg = cJSON_CreateObject();
    cJSON *usr_msg = cJSON_CreateObject();
    if (!sys_msg || !usr_msg) {
        cJSON_Delete(messages);
        cJSON_Delete(root);
        return "";
    }

    cJSON_AddStringToObject(sys_msg, "role", "system");
    cJSON_AddStringToObject(sys_msg, "content", system_prompt.c_str());
    cJSON_AddItemToArray(messages, sys_msg);

    for (const auto& history : get_llm_conversation()) {
        cJSON* msg = cJSON_CreateObject();
        if (!msg) continue;
        cJSON_AddStringToObject(msg, "role", history.role.c_str());
        cJSON_AddStringToObject(msg, "content", history.content.c_str());
        cJSON_AddItemToArray(messages, msg);
    }

    cJSON_AddStringToObject(usr_msg, "role", "user");
    cJSON_AddStringToObject(usr_msg, "content", user_text.c_str());
    cJSON_AddItemToArray(messages, usr_msg);

    cJSON_AddItemToObject(root, "messages", messages);

    char *json_string = cJSON_PrintUnformatted(root);
    std::string result(json_string ? json_string : "");

    if (json_string) free(json_string);
    cJSON_Delete(root);

    ESP_LOGI(TAG, "LLM request built, length=%d", (int)result.length());
    return result;
}

} // namespace ai
} // namespace smart_fridge
