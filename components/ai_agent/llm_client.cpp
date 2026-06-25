#include "ai_agent.hpp"

#include "cJSON.h"
#include "credentials_manager.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "llm_config.hpp"
#include "llm_internal.hpp"
#include "llm_types.hpp"
#include "llm_context.hpp"

#include <algorithm>
#include <cstring>
#include <string>

static const char *TAG = "LLMClient";

namespace smart_fridge {
namespace ai {

namespace {

struct buffered_http_context_t {
    std::string *response = nullptr;
};

struct sse_context_t {
    llm_stream_callbacks_t callbacks;
    std::string *content = nullptr;
    std::string pending;
    std::string raw;
    bool saw_sse = false;
    bool done = false;
    bool cancelled = false;
};

llm_error_t map_esp_error(esp_err_t error)
{
    switch (error) {
        case ESP_OK:
            return llm_error_t::OK;
        case ESP_ERR_HTTP_EAGAIN:
            return llm_error_t::ERR_TIMEOUT;
        case ESP_ERR_HTTP_CONNECT:
        case ESP_ERR_HTTP_CONNECTION_CLOSED:
            return llm_error_t::ERR_NETWORK;
        default:
            return llm_error_t::ERR_NETWORK;
    }
}

llm_error_t map_http_status(int status)
{
    if (status >= 200 && status < 300) {
        return llm_error_t::OK;
    }
    if (status >= 500) {
        return llm_error_t::ERR_HTTP_5XX;
    }
    return llm_error_t::ERR_HTTP_4XX;
}

esp_err_t buffered_http_event_handler(esp_http_client_event_t *event)
{
    auto *ctx = static_cast<buffered_http_context_t *>(event->user_data);
    if (ctx && ctx->response && event->event_id == HTTP_EVENT_ON_DATA &&
        event->data && event->data_len > 0) {
        ctx->response->append(static_cast<const char *>(event->data),
                              event->data_len);
    }
    return ESP_OK;
}

std::string trim(const std::string &value)
{
    const size_t start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return "";
    }
    const size_t end = value.find_last_not_of(" \t\r\n");
    return value.substr(start, end - start + 1);
}

bool process_sse_payload(sse_context_t *ctx, const std::string &payload)
{
    const std::string data = trim(payload);
    if (data.empty()) {
        return true;
    }
    if (data == "[DONE]") {
        ctx->done = true;
        return true;
    }

    cJSON *root = cJSON_Parse(data.c_str());
    if (!root) {
        ESP_LOGW(TAG, "Ignoring malformed SSE data frame");
        return true;
    }

    std::string delta_text;
    cJSON *choices = cJSON_GetObjectItem(root, "choices");
    if (cJSON_IsArray(choices) && cJSON_GetArraySize(choices) > 0) {
        cJSON *choice = cJSON_GetArrayItem(choices, 0);
        cJSON *delta = cJSON_GetObjectItem(choice, "delta");
        cJSON *content = delta ? cJSON_GetObjectItem(delta, "content")
                               : nullptr;
        if (cJSON_IsString(content) && content->valuestring) {
            delta_text = content->valuestring;
        }

        // Some OpenAI-compatible gateways use a complete message even when
        // stream=true. Accept it as one final delta.
        if (delta_text.empty()) {
            cJSON *message = cJSON_GetObjectItem(choice, "message");
            content =
                message ? cJSON_GetObjectItem(message, "content") : nullptr;
            if (cJSON_IsString(content) && content->valuestring) {
                delta_text = content->valuestring;
            }
        }
    }

    if (delta_text.empty()) {
        cJSON *content = cJSON_GetObjectItem(root, "content");
        if (cJSON_IsString(content) && content->valuestring) {
            delta_text = content->valuestring;
        }
    }
    cJSON_Delete(root);

    if (!delta_text.empty()) {
        ctx->content->append(delta_text);
        if (ctx->callbacks.on_delta &&
            !ctx->callbacks.on_delta(delta_text)) {
            ctx->cancelled = true;
            return false;
        }
    }
    return true;
}

bool process_sse_event(sse_context_t *ctx, const std::string &event)
{
    std::string combined;
    size_t offset = 0;
    while (offset < event.size()) {
        const size_t end = event.find('\n', offset);
        std::string line =
            event.substr(offset, end == std::string::npos
                                     ? std::string::npos
                                     : end - offset);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.rfind("data:", 0) == 0) {
            ctx->saw_sse = true;
            if (!combined.empty()) {
                combined.push_back('\n');
            }
            combined.append(line.substr(5));
        }
        if (end == std::string::npos) {
            break;
        }
        offset = end + 1;
    }
    return combined.empty() || process_sse_payload(ctx, combined);
}

esp_err_t sse_http_event_handler(esp_http_client_event_t *event)
{
    auto *ctx = static_cast<sse_context_t *>(event->user_data);
    if (!ctx) {
        return ESP_OK;
    }
    if ((ctx->callbacks.is_cancelled &&
         ctx->callbacks.is_cancelled()) ||
        ctx->cancelled) {
        ctx->cancelled = true;
        return ESP_FAIL;
    }

    if (event->event_id != HTTP_EVENT_ON_DATA || !event->data ||
        event->data_len <= 0) {
        return ESP_OK;
    }

    if (ctx->raw.size() < 4096) {
        const size_t copy =
            std::min<size_t>(event->data_len, 4096 - ctx->raw.size());
        ctx->raw.append(static_cast<const char *>(event->data), copy);
    }
    ctx->pending.append(static_cast<const char *>(event->data),
                        event->data_len);

    while (true) {
        size_t separator = ctx->pending.find("\n\n");
        size_t separator_size = 2;
        const size_t crlf_separator = ctx->pending.find("\r\n\r\n");
        if (crlf_separator != std::string::npos &&
            (separator == std::string::npos ||
             crlf_separator < separator)) {
            separator = crlf_separator;
            separator_size = 4;
        }
        if (separator == std::string::npos) {
            break;
        }

        const std::string complete_event =
            ctx->pending.substr(0, separator);
        ctx->pending.erase(0, separator + separator_size);
        if (!process_sse_event(ctx, complete_event)) {
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

esp_http_client_handle_t configure_client(
    const char *url, const char *api_key, http_event_handle_cb handler,
    void *user_data)
{
    esp_http_client_config_t config = {};
    config.url = url;
    config.event_handler = handler;
    config.user_data = user_data;
    config.timeout_ms = config::AI_LLM_TOTAL_TIMEOUT_MS;
    config.buffer_size = 2048;
    config.buffer_size_tx = 2048;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return nullptr;
    }
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Accept", "text/event-stream");
    const std::string authorization = std::string("Bearer ") + api_key;
    esp_http_client_set_header(client, "Authorization",
                               authorization.c_str());
    return client;
}

}  // namespace

llm_error_t http_perform_llm(const std::string &request_body,
                             std::string &out_raw_response,
                             int &out_http_status)
{
    out_raw_response.clear();
    out_http_status = 0;
    const char *url = credentials_get_llm_api_url();
    const char *key = credentials_get_llm_api_key();
    if (!url || !url[0] || !key || !key[0]) {
        return llm_error_t::ERR_NO_API_KEY;
    }

    buffered_http_context_t context{&out_raw_response};
    esp_http_client_handle_t client =
        configure_client(url, key, buffered_http_event_handler, &context);
    if (!client) {
        return llm_error_t::ERR_MEMORY;
    }
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_post_field(client, request_body.c_str(),
                                   request_body.length());

    esp_err_t error;
    do {
        error = esp_http_client_perform(client);
    } while (error == ESP_ERR_HTTP_EAGAIN);

    out_http_status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (error != ESP_OK) {
        return map_esp_error(error);
    }
    return map_http_status(out_http_status);
}

llm_error_t http_perform_llm_stream(
    const std::string &request_body,
    const llm_stream_callbacks_t &callbacks, std::string &out_content,
    int &out_http_status)
{
    out_content.clear();
    out_http_status = 0;
    const char *url = credentials_get_llm_api_url();
    const char *key = credentials_get_llm_api_key();
    if (!url || !url[0] || !key || !key[0]) {
        return llm_error_t::ERR_NO_API_KEY;
    }

    sse_context_t context;
    context.callbacks = callbacks;
    context.content = &out_content;
    esp_http_client_handle_t client =
        configure_client(url, key, sse_http_event_handler, &context);
    if (!client) {
        return llm_error_t::ERR_MEMORY;
    }
    esp_http_client_set_post_field(client, request_body.c_str(),
                                   request_body.length());

    esp_err_t error;
    do {
        error = esp_http_client_perform(client);
    } while (error == ESP_ERR_HTTP_EAGAIN &&
             !(callbacks.is_cancelled && callbacks.is_cancelled()));

    if (!context.pending.empty() && !context.cancelled) {
        process_sse_event(&context, context.pending);
    }
    out_http_status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (context.cancelled ||
        (callbacks.is_cancelled && callbacks.is_cancelled())) {
        return llm_error_t::ERR_NETWORK;
    }
    if (error != ESP_OK) {
        return map_esp_error(error);
    }

    const llm_error_t status_error = map_http_status(out_http_status);
    if (status_error != llm_error_t::OK) {
        ESP_LOGE(TAG, "SSE HTTP %d: %s", out_http_status,
                 context.raw.c_str());
        return status_error;
    }

    // Tolerate a gateway that ignored stream=true and returned normal JSON.
    if (!context.saw_sse && out_content.empty() && !context.raw.empty()) {
        cJSON *root = cJSON_Parse(context.raw.c_str());
        if (root) {
            cJSON *choices = cJSON_GetObjectItem(root, "choices");
            cJSON *choice =
                cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0)
                                       : nullptr;
            cJSON *message =
                choice ? cJSON_GetObjectItem(choice, "message") : nullptr;
            cJSON *content =
                message ? cJSON_GetObjectItem(message, "content") : nullptr;
            if (cJSON_IsString(content) && content->valuestring) {
                out_content = content->valuestring;
                if (callbacks.on_delta) {
                    callbacks.on_delta(out_content);
                }
            }
            cJSON_Delete(root);
        }
    }

    return out_content.empty() ? llm_error_t::ERR_LLM_REFUSAL
                               : llm_error_t::OK;
}

llm_error_t call_llm_api(const std::string &user_text,
                         std::string &out_reply)
{
    out_reply.clear();
    if (handle_pending_inventory_confirmation(user_text, out_reply) ==
        pending_confirmation_result_t::HANDLED) {
        return llm_error_t::OK;
    }

    const std::string request_body = build_llm_request(user_text, false);
    if (request_body.empty()) {
        return llm_error_t::ERR_MEMORY;
    }

    std::string raw_response;
    int http_status = 0;
    llm_error_t error =
        http_perform_llm(request_body, raw_response, http_status);
    if (error != llm_error_t::OK) {
        return error;
    }

    llm_action_t action;
    error = parse_llm_response(raw_response, action);
    if (error != llm_error_t::OK) {
        return error;
    }
    error = execute_llm_action(action, out_reply);
    if (error == llm_error_t::OK) {
        append_llm_conversation_turn(user_text, out_reply);
    }
    return error;
}

}  // namespace ai
}  // namespace smart_fridge
