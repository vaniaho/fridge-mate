#include "ai_agent.hpp"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "llm_config.hpp"
#include "llm_internal.hpp"
#include "llm_context.hpp"
#include "llm_types.hpp"
#include "llm_utils.hpp"
#include "system_events.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static const char *TAG = "AIWorker";

namespace smart_fridge {
namespace ai {

namespace {

QueueHandle_t s_ai_request_queue = nullptr;
TaskHandle_t s_ai_worker_task = nullptr;
llm_stats_t s_llm_stats = {};
std::atomic<uint32_t> s_cancel_generation{1};

struct ai_request_t {
    char *text;
    uint32_t generation;
};

std::string trim(const std::string &value)
{
    const size_t start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return "";
    }
    const size_t end = value.find_last_not_of(" \t\r\n");
    return value.substr(start, end - start + 1);
}

bool consume_prefix(std::string &value, const char *prefix)
{
    const size_t length = strlen(prefix);
    if (value.compare(0, length, prefix) != 0) return false;
    value.erase(0, length);
    value = trim(value);
    return true;
}

bool consume_suffix(std::string &value, const char *suffix)
{
    const size_t length = strlen(suffix);
    if (value.size() < length ||
        value.compare(value.size() - length, length, suffix) != 0) {
        return false;
    }
    value.erase(value.size() - length);
    value = trim(value);
    return true;
}

std::string normalize_inventory_text(std::string text)
{
    text = trim(text);
    bool changed = true;
    while (changed && !text.empty()) {
        changed = false;
        static const char *prefixes[] = {
            "帮我", "请帮我", "请", "麻烦你", "麻烦", "给我", "把", "再",
            "顺便", "小鲜", "小鲜小鲜",
        };
        for (const char *prefix : prefixes) {
            if (consume_prefix(text, prefix)) {
                changed = true;
                break;
            }
        }
    }

    changed = true;
    while (changed && !text.empty()) {
        changed = false;
        static const char *suffixes[] = {
            "。", "！", "？", "；", ".", ",", "!", "?", "一下", "吧", "呀",
            "呢", "谢谢", "到冰箱里", "进冰箱", "放冰箱",
        };
        for (const char *suffix : suffixes) {
            if (consume_suffix(text, suffix)) {
                changed = true;
                break;
            }
        }
    }
    return text;
}

int chinese_number_value(const std::string &text)
{
    struct token_t {
        const char *text;
        int value;
        bool unit;
    };
    static const token_t tokens[] = {
        {"零", 0, false}, {"一", 1, false}, {"二", 2, false},
        {"两", 2, false}, {"三", 3, false}, {"四", 4, false},
        {"五", 5, false}, {"六", 6, false}, {"七", 7, false},
        {"八", 8, false}, {"九", 9, false}, {"十", 10, true},
        {"百", 100, true},
    };

    int total = 0;
    int current = 0;
    size_t offset = 0;
    while (offset < text.size()) {
        bool matched = false;
        for (const auto &token : tokens) {
            const size_t length = strlen(token.text);
            if (text.compare(offset, length, token.text) != 0) continue;
            matched = true;
            offset += length;
            if (!token.unit) {
                current = token.value;
            } else {
                if (current == 0) current = 1;
                total += current * token.value;
                current = 0;
            }
            break;
        }
        if (!matched) return 0;
    }
    return total + current;
}

bool parse_quantity_and_item(std::string segment, int &quantity,
                             std::string &item, std::string &unit)
{
    segment = normalize_inventory_text(std::move(segment));
    if (segment.empty()) return false;

    size_t number_length = 0;
    if (std::isdigit(static_cast<unsigned char>(segment[0]))) {
        while (number_length < segment.size() &&
               std::isdigit(static_cast<unsigned char>(
                   segment[number_length]))) {
            ++number_length;
        }
        quantity = atoi(segment.substr(0, number_length).c_str());
    } else {
        static const char *number_tokens[] = {
            "零", "一", "二", "两", "三", "四", "五",
            "六", "七", "八", "九", "十", "百",
        };
        while (number_length < segment.size()) {
            bool matched = false;
            for (const char *token : number_tokens) {
                const size_t length = strlen(token);
                if (segment.compare(number_length, length, token) == 0) {
                    number_length += length;
                    matched = true;
                    break;
                }
            }
            if (!matched) break;
        }
        quantity = number_length > 0
            ? chinese_number_value(segment.substr(0, number_length))
            : 1;
    }
    if (quantity <= 0) return false;

    item = trim(segment.substr(number_length));
    unit.clear();
    static const char *classifiers[] = {
        "个", "只", "盒", "瓶", "袋", "颗", "根", "块", "斤", "公斤",
        "包", "罐", "条", "片", "份", "把", "棵", "串", "杯", "升",
        "毫升", "克",
    };
    for (const char *classifier : classifiers) {
        if (consume_prefix(item, classifier)) {
            unit = classifier;
            break;
        }
    }
    item = normalize_inventory_text(item);
    static const char *bad_item_prefixes[] = {
        "去", "进去", "到", "入", "进", "出", "出来",
    };
    for (const char *prefix : bad_item_prefixes) {
        if (consume_prefix(item, prefix)) break;
    }
    return !item.empty();
}

std::vector<std::string> split_inventory_items(const std::string &text)
{
    static const char *separators[] = {
        "以及", "还有", "再来", "另外", "顺便", "和", "、", "，", ",",
    };
    std::vector<std::string> result;
    size_t offset = 0;
    while (offset < text.size()) {
        size_t best = std::string::npos;
        size_t separator_length = 0;
        for (const char *separator : separators) {
            const size_t found = text.find(separator, offset);
            if (found != std::string::npos &&
                (best == std::string::npos || found < best ||
                 (found == best && strlen(separator) >
                                       separator_length))) {
                best = found;
                separator_length = strlen(separator);
            }
        }
        if (best == std::string::npos) {
            result.push_back(text.substr(offset));
            break;
        }
        result.push_back(text.substr(offset, best - offset));
        offset = best + separator_length;
    }
    return result;
}

bool try_parse_local_inventory_command(const std::string &input,
                                       llm_action_t &action)
{
    struct verb_t {
        const char *text;
        llm_action_type_t action;
    };
    static const verb_t verbs[] = {
        {"拿出来", llm_action_type_t::REMOVE},
        {"取出", llm_action_type_t::REMOVE},
        {"拿出", llm_action_type_t::REMOVE},
        {"移除", llm_action_type_t::REMOVE},
        {"删除", llm_action_type_t::REMOVE},
        {"消耗", llm_action_type_t::REMOVE},
        {"吃掉", llm_action_type_t::REMOVE},
        {"用掉", llm_action_type_t::REMOVE},
        {"放进去", llm_action_type_t::ADD},
        {"放到冰箱里", llm_action_type_t::ADD},
        {"放冰箱", llm_action_type_t::ADD},
        {"放入", llm_action_type_t::ADD},
        {"放进", llm_action_type_t::ADD},
        {"存进去", llm_action_type_t::ADD},
        {"存入", llm_action_type_t::ADD},
        {"加入", llm_action_type_t::ADD},
        {"加进去", llm_action_type_t::ADD},
        {"添加", llm_action_type_t::ADD},
    };

    const verb_t *matched_verb = nullptr;
    size_t verb_offset = std::string::npos;
    size_t verb_length = 0;
    for (const auto &verb : verbs) {
        const size_t found = input.find(verb.text);
        if (found != std::string::npos &&
            (verb_offset == std::string::npos || found < verb_offset ||
             (found == verb_offset && strlen(verb.text) > verb_length))) {
            matched_verb = &verb;
            verb_offset = found;
            verb_length = strlen(verb.text);
        }
    }
    if (!matched_verb) return false;

    std::string tail = normalize_inventory_text(
        input.substr(verb_offset + verb_length));
    if (tail.empty() && verb_offset > 0) {
        tail = normalize_inventory_text(input.substr(0, verb_offset));
    }
    std::vector<llm_action_t> parsed;
    for (const auto &segment : split_inventory_items(tail)) {
        int quantity = 0;
        std::string item;
        std::string unit;
        if (!parse_quantity_and_item(segment, quantity, item, unit)) {
            return false;
        }
        llm_action_t child;
        child.type = matched_verb->action;
        child.target_item = item;
        child.quantity = quantity;
        child.unit = unit;
        parsed.push_back(std::move(child));
    }
    if (parsed.empty()) return false;
    if (parsed.size() == 1) {
        action = std::move(parsed.front());
    } else {
        action = {};
        action.type = llm_action_type_t::BATCH;
        action.actions = std::move(parsed);
    }
    return true;
}

void append_utf8(std::string &out, uint32_t codepoint)
{
    if (codepoint <= 0x7f) {
        out.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7ff) {
        out.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else {
        out.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    }
}

int hex_value(char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

bool extract_json_string_field(const std::string &json, const char *field,
                               std::string &decoded, bool &complete)
{
    decoded.clear();
    complete = false;
    const std::string key = std::string("\"") + field + "\"";
    size_t offset = json.find(key);
    if (offset == std::string::npos) {
        return false;
    }
    offset = json.find(':', offset + key.size());
    if (offset == std::string::npos) {
        return false;
    }
    offset = json.find('"', offset + 1);
    if (offset == std::string::npos) {
        return false;
    }
    ++offset;

    while (offset < json.size()) {
        const char ch = json[offset++];
        if (ch == '"') {
            complete = true;
            return true;
        }
        if (ch != '\\') {
            decoded.push_back(ch);
            continue;
        }
        if (offset >= json.size()) {
            return true;
        }

        const char escaped = json[offset++];
        switch (escaped) {
            case '"': decoded.push_back('"'); break;
            case '\\': decoded.push_back('\\'); break;
            case '/': decoded.push_back('/'); break;
            case 'b': decoded.push_back('\b'); break;
            case 'f': decoded.push_back('\f'); break;
            case 'n': decoded.push_back('\n'); break;
            case 'r': decoded.push_back('\r'); break;
            case 't': decoded.push_back('\t'); break;
            case 'u': {
                if (offset + 4 > json.size()) {
                    return true;
                }
                uint32_t codepoint = 0;
                for (int i = 0; i < 4; ++i) {
                    const int digit = hex_value(json[offset + i]);
                    if (digit < 0) {
                        return true;
                    }
                    codepoint = (codepoint << 4) | digit;
                }
                offset += 4;
                append_utf8(decoded, codepoint);
                break;
            }
            default:
                decoded.push_back(escaped);
                break;
        }
    }
    return true;
}

llm_action_type_t action_from_text(const std::string &action)
{
    if (action == "CHAT") return llm_action_type_t::CHAT;
    if (action == "RECIPE") return llm_action_type_t::RECIPE;
    if (action == "ADD") return llm_action_type_t::ADD;
    if (action == "REMOVE") return llm_action_type_t::REMOVE;
    if (action == "BATCH") return llm_action_type_t::BATCH;
    return llm_action_type_t::UNKNOWN;
}

bool dispatch_tts_sentence(const std::string &sentence);
bool dispatch_tts_control(sys_event_type_t event);

class sentence_buffer_t {
public:
    using callback_t = std::function<bool(const std::string &)>;

    explicit sentence_buffer_t(callback_t callback)
        : callback_(std::move(callback))
    {
    }

    bool feed(const std::string &text)
    {
        pending_.append(text);
        return emit_complete_sentences();
    }

    bool finish()
    {
        if (!emit_complete_sentences()) {
            return false;
        }
        const std::string remainder = trim(pending_);
        pending_.clear();
        return remainder.empty() || callback_(remainder);
    }

private:
    bool emit_complete_sentences()
    {
        static const char *delimiters[] = {
            "。", "！", "？", "；", "\n", "!", "?", ";", ".",
        };
        while (true) {
            size_t best = std::string::npos;
            size_t delimiter_size = 0;
            for (const char *delimiter : delimiters) {
                const size_t found = pending_.find(delimiter);
                if (found != std::string::npos &&
                    (best == std::string::npos || found < best)) {
                    best = found;
                    delimiter_size = strlen(delimiter);
                }
            }
            if (best == std::string::npos) {
                return true;
            }

            std::string sentence =
                trim(pending_.substr(0, best + delimiter_size));
            pending_.erase(0, best + delimiter_size);
            if (!sentence.empty() && !callback_(sentence)) {
                return false;
            }
        }
    }

    callback_t callback_;
    std::string pending_;
};

class streamed_reply_t {
public:
    explicit streamed_reply_t(uint32_t generation, bool tts_active)
        : generation_(generation),
          tts_active_(tts_active),
          sentences_([&](const std::string &sentence) {
              if (cancelled()) {
                  return false;
              }
              if (tts_active_ && !dispatch_tts_sentence(sentence)) {
                  return false;
              }
              dispatch_llm_stream_text_event(sentence);
              streamed_any_ = true;
              return true;
          })
    {
    }

    bool on_delta(const std::string &delta)
    {
        if (cancelled()) {
            return false;
        }
        content_.append(delta);

        std::string action_text;
        bool action_complete = false;
        if (extract_json_string_field(content_, "action", action_text,
                                      action_complete) &&
            action_complete) {
            action_ = action_from_text(action_text);
        }

        std::string reply;
        bool reply_complete = false;
        if (!extract_json_string_field(content_, "tts_reply", reply,
                                       reply_complete)) {
            return true;
        }

        // Inventory writes must wait for validation and explicit confirmation.
        if (action_ == llm_action_type_t::ADD ||
            action_ == llm_action_type_t::REMOVE ||
            action_ == llm_action_type_t::BATCH ||
            action_ == llm_action_type_t::UNKNOWN) {
            return true;
        }

        if (reply.size() > forwarded_bytes_) {
            const std::string new_text = reply.substr(forwarded_bytes_);
            forwarded_bytes_ = reply.size();
            return sentences_.feed(new_text);
        }
        return true;
    }

    bool finish(const llm_action_t &action)
    {
        if (action.type == llm_action_type_t::ADD ||
            action.type == llm_action_type_t::REMOVE ||
            action.type == llm_action_type_t::BATCH) {
            return true;
        }

        std::string reply;
        bool complete = false;
        if (extract_json_string_field(content_, "tts_reply", reply,
                                      complete)) {
            if (reply.size() > forwarded_bytes_ &&
                !sentences_.feed(reply.substr(forwarded_bytes_))) {
                return false;
            }
            forwarded_bytes_ = reply.size();
        }
        return sentences_.finish();
    }

    bool streamed_any() const { return streamed_any_; }
    const std::string &content() const { return content_; }

private:
    bool cancelled() const
    {
        return generation_ != s_cancel_generation.load();
    }

    uint32_t generation_;
    bool tts_active_;
    bool streamed_any_ = false;
    llm_action_type_t action_ = llm_action_type_t::UNKNOWN;
    size_t forwarded_bytes_ = 0;
    std::string content_;
    sentence_buffer_t sentences_;
};

bool dispatch_tts_sentence(const std::string &sentence)
{
    auto *payload = static_cast<llm_stream_text_payload_t *>(
        malloc(sizeof(llm_stream_text_payload_t)));
    if (!payload) {
        return false;
    }
    memset(payload, 0, sizeof(*payload));
    safe_copy_text(payload->text, sizeof(payload->text), sentence);
    if (send_system_event(EVT_TTS_STREAM_SENTENCE, payload,
                          sizeof(*payload)) != ESP_OK) {
        free(payload);
        return false;
    }
    return true;
}

bool dispatch_tts_control(sys_event_type_t event)
{
    return send_system_event(event, nullptr, 0) == ESP_OK;
}

llm_error_t call_llm_streaming(const std::string &input,
                               uint32_t generation,
                               std::string &out_reply)
{
    out_reply.clear();
    if (handle_pending_inventory_confirmation(input, out_reply) ==
        pending_confirmation_result_t::HANDLED) {
        return llm_error_t::OK;
    }

    llm_action_t local_action;
    if (try_parse_local_inventory_command(input, local_action)) {
        ESP_LOGI(TAG, "Inventory command handled locally (%u action%s)",
                 static_cast<unsigned>(
                     local_action.type == llm_action_type_t::BATCH
                         ? local_action.actions.size() : 1),
                 local_action.type == llm_action_type_t::BATCH ? "s" : "");
        const llm_error_t result =
            execute_llm_action(local_action, out_reply, false);
        if (result == llm_error_t::OK) {
            append_llm_conversation_turn(input, out_reply);
        }
        return result;
    }

    const std::string request = build_llm_request(input, true);
    if (request.empty()) {
        return llm_error_t::ERR_MEMORY;
    }

    const bool tts_active = dispatch_tts_control(EVT_TTS_STREAM_BEGIN);
    streamed_reply_t streamed(generation, tts_active);
    llm_stream_callbacks_t callbacks;
    callbacks.is_cancelled = [generation]() {
        return generation != s_cancel_generation.load();
    };
    callbacks.on_delta =
        [&](const std::string &delta) { return streamed.on_delta(delta); };

    std::string content;
    int http_status = 0;
    llm_error_t error =
        http_perform_llm_stream(request, callbacks, content, http_status);
    if (generation != s_cancel_generation.load()) {
        if (tts_active) {
            dispatch_tts_control(EVT_TTS_STREAM_CANCEL);
        }
        return llm_error_t::ERR_NETWORK;
    }
    if (error != llm_error_t::OK) {
        out_reply = llm_error_to_user_hint(error);
        if (tts_active) {
            dispatch_tts_sentence(out_reply);
            dispatch_llm_stream_text_event(out_reply);
            dispatch_tts_control(EVT_TTS_STREAM_END);
        }
        dispatch_llm_response_event(out_reply, UI_ACTION_NONE, tts_active);
        return error;
    }

    llm_action_t action;
    error = parse_llm_content(content, action);
    if (error != llm_error_t::OK) {
        out_reply = llm_error_to_user_hint(error);
        if (tts_active) {
            dispatch_tts_sentence(out_reply);
            dispatch_llm_stream_text_event(out_reply);
            dispatch_tts_control(EVT_TTS_STREAM_END);
        }
        dispatch_llm_response_event(out_reply, UI_ACTION_NONE, tts_active);
        return error;
    }

    if (!streamed.finish(action)) {
        if (tts_active) {
            dispatch_tts_control(EVT_TTS_STREAM_CANCEL);
        }
        return llm_error_t::ERR_QUEUE_FULL;
    }

    const bool reply_already_queued = streamed.streamed_any();
    error = execute_llm_action(action, out_reply, tts_active);
    if (error != llm_error_t::OK) {
        if (tts_active) {
            dispatch_tts_control(EVT_TTS_STREAM_CANCEL);
        }
        return error;
    }

    if (!reply_already_queued && tts_active) {
        dispatch_tts_sentence(out_reply);
        dispatch_llm_stream_text_event(out_reply);
    }
    if (tts_active) {
        dispatch_tts_control(EVT_TTS_STREAM_END);
    }
    append_llm_conversation_turn(input, out_reply);
    return llm_error_t::OK;
}

void drain_request_queue()
{
    if (!s_ai_request_queue) {
        return;
    }
    ai_request_t request = {};
    while (xQueueReceive(s_ai_request_queue, &request, 0) == pdTRUE) {
        free(request.text);
    }
}

static void ai_worker_task(void *parameters)
{
    (void)parameters;
    ai_request_t request = {};
    while (true) {
        if (xQueueReceive(s_ai_request_queue, &request, portMAX_DELAY) !=
            pdTRUE) {
            continue;
        }
        if (!request.text) {
            continue;
        }

        std::string input(request.text);
        free(request.text);
        request.text = nullptr;
        if (request.generation != s_cancel_generation.load()) {
            continue;
        }

        const TickType_t start = xTaskGetTickCount();
        std::string reply;
        const llm_error_t error =
            call_llm_streaming(input, request.generation, reply);
        const uint32_t latency_ms =
            (xTaskGetTickCount() - start) * 1000 / configTICK_RATE_HZ;

        s_llm_stats.total_calls++;
        s_llm_stats.last_latency_ms = latency_ms;
        s_llm_stats.last_error = error;
        if (error != llm_error_t::OK &&
            request.generation == s_cancel_generation.load()) {
            s_llm_stats.failed_calls++;
            ESP_LOGE(TAG, "Streaming LLM failed: %d", (int)error);
            if (reply.empty()) {
                dispatch_llm_response_event(llm_error_to_user_hint(error),
                                            UI_ACTION_NONE);
            }
        }
    }
}

}  // namespace

bool call_llm_api_async(const std::string &user_voice_text)
{
    if (!s_ai_request_queue) {
        s_ai_request_queue =
            xQueueCreate(config::AI_WORKER_QUEUE_LEN, sizeof(ai_request_t));
        if (!s_ai_request_queue) {
            return false;
        }
    }
    if (!s_ai_worker_task) {
        if (xTaskCreate(ai_worker_task, "ai_worker",
                        config::AI_WORKER_STACK_SIZE, nullptr, 4,
                        &s_ai_worker_task) != pdPASS) {
            return false;
        }
    }

    ai_request_t request = {};
    request.text = strdup(user_voice_text.c_str());
    request.generation = s_cancel_generation.load();
    if (!request.text) {
        return false;
    }
    if (xQueueSend(s_ai_request_queue, &request, pdMS_TO_TICKS(100)) !=
        pdTRUE) {
        free(request.text);
        return false;
    }
    return true;
}

void cancel_llm_api(void)
{
    s_cancel_generation.fetch_add(1);
    drain_request_queue();
}

llm_stats_t get_llm_stats(void)
{
    return s_llm_stats;
}

}  // namespace ai
}  // namespace smart_fridge
