#include "realtime_client.hpp"

#include "cJSON.h"
#include "credentials_manager.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_random.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "volc_binary_protocol.hpp"

#include <atomic>
#include <string>
#include <vector>

namespace smart_fridge {
namespace audio {

namespace {

constexpr EventBits_t WS_CONNECTED = BIT0;
constexpr EventBits_t CONNECTION_STARTED = BIT1;
constexpr EventBits_t SESSION_STARTED = BIT2;
constexpr EventBits_t FAILED = BIT3;
constexpr EventBits_t TURN_FINISHED = BIT4;

struct options_t {
    std::string appid;
    std::string resource_id = "volc.speech.dialog";
    std::string app_key = "PlgvMymc7f3tQnJ6";
    std::string speaker = "zh_female_vv_jupiter_bigtts";
    std::string input_mod = "keep_alive";
    int sample_rate = 24000;
};

struct context_t {
    EventGroupHandle_t events = nullptr;
    SemaphoreHandle_t mutex = nullptr;
    realtime_callbacks_t callbacks;
    std::vector<uint8_t> frame;
    std::string error;
    std::atomic<bool> interrupt_requested{false};
    int output_sample_rate = 24000;
};

std::string random_id()
{
    char id[37];
    snprintf(id, sizeof(id), "%08lx-%04lx-%04lx-%04lx-%08lx%04lx",
             static_cast<unsigned long>(esp_random()),
             static_cast<unsigned long>(esp_random() & 0xffff),
             static_cast<unsigned long>(esp_random() & 0xffff),
             static_cast<unsigned long>(esp_random() & 0xffff),
             static_cast<unsigned long>(esp_random()),
             static_cast<unsigned long>(esp_random() & 0xffff));
    return id;
}

options_t parse_options(const char *extra)
{
    options_t options;
    cJSON *root = extra && extra[0] ? cJSON_Parse(extra) : nullptr;
    if (!root) return options;
    auto read_string = [&](const char *name, std::string &value) {
        cJSON *item = cJSON_GetObjectItem(root, name);
        if (cJSON_IsString(item) && item->valuestring) {
            value = item->valuestring;
        }
    };
    read_string("appid", options.appid);
    read_string("resource_id", options.resource_id);
    read_string("app_key", options.app_key);
    read_string("speaker", options.speaker);
    read_string("input_mod", options.input_mod);
    cJSON *rate = cJSON_GetObjectItem(root, "sample_rate");
    if (cJSON_IsNumber(rate)) options.sample_rate = rate->valueint;
    cJSON_Delete(root);
    return options;
}

std::string session_json(const voice_model_config_t &config,
                         const options_t &options)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *asr = cJSON_CreateObject();
    cJSON *audio = cJSON_CreateObject();
    cJSON *asr_extra = cJSON_CreateObject();
    cJSON *dialog = cJSON_CreateObject();
    cJSON *dialog_extra = cJSON_CreateObject();
    cJSON *tts = cJSON_CreateObject();
    cJSON *tts_audio = cJSON_CreateObject();
    cJSON *tts_extra = cJSON_CreateObject();
    if (!root || !asr || !audio || !asr_extra || !dialog ||
        !dialog_extra || !tts || !tts_audio || !tts_extra) {
        cJSON_Delete(root);
        return "";
    }

    cJSON_AddStringToObject(audio, "format", "pcm");
    cJSON_AddNumberToObject(audio, "sample_rate", 16000);
    cJSON_AddNumberToObject(audio, "channel", 1);
    cJSON_AddItemToObject(asr, "audio_info", audio);
    cJSON_AddNumberToObject(asr_extra, "end_smooth_window_ms", 800);
    cJSON_AddBoolToObject(asr_extra, "enable_custom_vad", true);
    cJSON_AddBoolToObject(asr_extra, "enable_asr_twopass", true);
    cJSON_AddItemToObject(asr, "extra", asr_extra);

    cJSON_AddStringToObject(dialog, "bot_name", "小鲜");
    cJSON_AddStringToObject(
        dialog, "system_role",
        "你是小鲜智能冰箱语音助手。回答简洁自然。库存存取、库存查询、"
        "临期查询和食谱推荐会由设备本地业务工具处理。");
    cJSON_AddStringToObject(dialog, "speaking_style",
                           "友好、清晰，每次尽量不超过三句话。");
    cJSON_AddStringToObject(dialog_extra, "input_mod",
                           options.input_mod.c_str());
    cJSON_AddStringToObject(dialog_extra, "model", config.model);
    cJSON_AddBoolToObject(dialog_extra, "enable_user_query_exit", true);
    cJSON_AddBoolToObject(dialog_extra, "enable_conversation_truncate",
                         true);
    cJSON_AddItemToObject(dialog, "extra", dialog_extra);

    cJSON_AddStringToObject(tts, "speaker", options.speaker.c_str());
    cJSON_AddStringToObject(tts_audio, "format", "pcm_s16le");
    cJSON_AddNumberToObject(tts_audio, "sample_rate", options.sample_rate);
    cJSON_AddNumberToObject(tts_audio, "channel", 1);
    cJSON_AddItemToObject(tts, "audio_config", tts_audio);
    cJSON_AddItemToObject(tts, "extra", tts_extra);

    cJSON_AddItemToObject(root, "asr", asr);
    cJSON_AddItemToObject(root, "dialog", dialog);
    cJSON_AddItemToObject(root, "tts", tts);
    char *json = cJSON_PrintUnformatted(root);
    std::string result = json ? json : "";
    free(json);
    cJSON_Delete(root);
    return result;
}

bool send_packet(esp_websocket_client_handle_t client,
                 const std::vector<uint8_t> &packet)
{
    return client && !packet.empty() &&
           esp_websocket_client_send_bin(
               client, reinterpret_cast<const char *>(packet.data()),
               packet.size(), pdMS_TO_TICKS(2000)) ==
               static_cast<int>(packet.size());
}

void fail(context_t *context, const std::string &error)
{
    xSemaphoreTake(context->mutex, portMAX_DELAY);
    context->error = error;
    xSemaphoreGive(context->mutex);
    if (context->callbacks.on_error) context->callbacks.on_error(error);
    xEventGroupSetBits(context->events, FAILED);
}

std::string payload_string(const volc::packet_t &packet)
{
    return std::string(
        reinterpret_cast<const char *>(packet.payload.data()),
        packet.payload.size());
}

void handle_json_event(context_t *context, const volc::packet_t &packet)
{
    const std::string payload = payload_string(packet);
    cJSON *root = payload.empty() ? nullptr : cJSON_Parse(payload.c_str());
    switch (packet.event) {
        case 50:
            xEventGroupSetBits(context->events, CONNECTION_STARTED);
            break;
        case 51:
        case 153: {
            cJSON *item = root ? cJSON_GetObjectItem(root, "error") : nullptr;
            fail(context, cJSON_IsString(item) && item->valuestring
                              ? item->valuestring
                              : "端到端语音服务返回错误");
            break;
        }
        case 150:
            xEventGroupSetBits(context->events, SESSION_STARTED);
            break;
        case 359:
            xEventGroupSetBits(context->events, TURN_FINISHED);
            if (context->callbacks.on_turn_finished) {
                context->callbacks.on_turn_finished();
            }
            break;
        case 450:
            if (context->callbacks.on_barge_in) {
                context->callbacks.on_barge_in();
            }
            break;
        case 451: {
            cJSON *results = root ? cJSON_GetObjectItem(root, "results")
                                  : nullptr;
            cJSON *first = cJSON_IsArray(results)
                               ? cJSON_GetArrayItem(results, 0)
                               : nullptr;
            cJSON *text = first ? cJSON_GetObjectItem(first, "text")
                                : nullptr;
            cJSON *interim =
                first ? cJSON_GetObjectItem(first, "is_interim") : nullptr;
            if (cJSON_IsString(text) && text->valuestring &&
                context->callbacks.on_asr) {
                if (context->callbacks.on_asr(text->valuestring,
                    !(cJSON_IsTrue(interim) ||
                      (cJSON_IsNumber(interim) && interim->valueint)))) {
                    context->interrupt_requested.store(true);
                }
            }
            break;
        }
        case 550: {
            cJSON *text =
                root ? cJSON_GetObjectItem(root, "content") : nullptr;
            if (cJSON_IsString(text) && text->valuestring &&
                context->callbacks.on_chat_text) {
                context->callbacks.on_chat_text(text->valuestring);
            }
            break;
        }
        default:
            break;
    }
    cJSON_Delete(root);
}

void handle_packet(context_t *context, const std::vector<uint8_t> &frame)
{
    volc::packet_t packet;
    std::string error;
    if (!volc::decode_packet(frame.data(), frame.size(), packet, error)) {
        fail(context, error);
        return;
    }
    if (packet.type == volc::message_type_t::ERROR) {
        fail(context, payload_string(packet));
        return;
    }
    if (packet.event == 352 ||
        packet.type == volc::message_type_t::AUDIO_SERVER) {
        if (!packet.payload.empty() && context->callbacks.on_audio &&
            !context->callbacks.on_audio(packet.payload.data(),
                                         packet.payload.size(),
                                         context->output_sample_rate)) {
            fail(context, "端到端语音播放缓冲区已满");
        }
        return;
    }
    handle_json_event(context, packet);
}

void websocket_handler(void *args, esp_event_base_t, int32_t event_id,
                       void *event_data)
{
    auto *context = static_cast<context_t *>(args);
    auto *data = static_cast<esp_websocket_event_data_t *>(event_data);
    if (event_id == WEBSOCKET_EVENT_CONNECTED) {
        xEventGroupSetBits(context->events, WS_CONNECTED);
    } else if (event_id == WEBSOCKET_EVENT_ERROR) {
        fail(context, "端到端语音 WebSocket 连接错误");
    } else if (event_id == WEBSOCKET_EVENT_DATA && data &&
               data->data_ptr && data->data_len > 0) {
        if (data->payload_offset == 0) {
            context->frame.clear();
            context->frame.reserve(data->payload_len);
        }
        context->frame.insert(
            context->frame.end(),
            reinterpret_cast<const uint8_t *>(data->data_ptr),
            reinterpret_cast<const uint8_t *>(data->data_ptr) +
                data->data_len);
        if (data->payload_offset + data->data_len >= data->payload_len) {
            handle_packet(context, context->frame);
            context->frame.clear();
        }
    }
}

bool wait_for(context_t &context, EventBits_t bit, int timeout_ms)
{
    EventBits_t result = xEventGroupWaitBits(
        context.events, bit | FAILED, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(timeout_ms));
    return (result & bit) != 0 && (result & FAILED) == 0;
}

}  // namespace

realtime_result_t realtime_dialogue_run(
    const realtime_audio_source_t &source,
    const realtime_callbacks_t &callbacks,
    const realtime_cancel_callback_t &is_cancelled,
    bool push_to_talk)
{
    realtime_result_t result;
    const voice_model_config_t *stored =
        credentials_get_voice_model_config(VOICE_MODEL_REALTIME);
    if (!stored) {
        result.error_hint = "端到端语音配置不存在";
        return result;
    }
    const voice_model_config_t config = *stored;
    options_t options = parse_options(config.extra);
    if (push_to_talk) {
        options.input_mod = "push_to_talk";
    }
    if (!config.url[0] || !config.key[0] || options.appid.empty()) {
        result.error_hint = "端到端语音 URL、Access Key 或 App ID 未配置";
        return result;
    }
    if (options.sample_rate != 24000) {
        result.error_hint = "端到端语音输出采样率必须配置为 24000 Hz";
        return result;
    }

    const std::string session_id = random_id();
    const std::string headers =
        "X-Api-App-ID: " + options.appid + "\r\n" +
        "X-Api-Access-Key: " + config.key + "\r\n" +
        "X-Api-Resource-Id: " + options.resource_id + "\r\n" +
        "X-Api-App-Key: " + options.app_key + "\r\n" +
        "X-Api-Connect-Id: " + random_id() + "\r\n";

    context_t context;
    context.events = xEventGroupCreate();
    context.mutex = xSemaphoreCreateMutex();
    context.callbacks = callbacks;
    context.output_sample_rate = options.sample_rate;
    if (!context.events || !context.mutex) {
        result.error_hint = "端到端语音会话内存不足";
        if (context.events) vEventGroupDelete(context.events);
        if (context.mutex) vSemaphoreDelete(context.mutex);
        return result;
    }

    esp_websocket_client_config_t config_ws = {};
    config_ws.uri = config.url;
    config_ws.headers = headers.c_str();
    config_ws.crt_bundle_attach = esp_crt_bundle_attach;
    config_ws.network_timeout_ms = 10000;
    config_ws.disable_auto_reconnect = true;
    config_ws.buffer_size = 4096;
    config_ws.task_stack = 10240;
    esp_websocket_client_handle_t client =
        esp_websocket_client_init(&config_ws);
    if (!client) {
        result.error_hint = "无法创建端到端语音 WebSocket";
        vSemaphoreDelete(context.mutex);
        vEventGroupDelete(context.events);
        return result;
    }
    esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY,
                                  websocket_handler, &context);

    bool ready = esp_websocket_client_start(client) == ESP_OK &&
                 wait_for(context, WS_CONNECTED, 10000);
    if (ready) {
        ready = send_packet(client, volc::encode_json_event(1, "", "{}")) &&
                wait_for(context, CONNECTION_STARTED, 10000);
    }
    if (ready) {
        const std::string start = session_json(config, options);
        ready = !start.empty() &&
                send_packet(client, volc::encode_json_event(
                                        100, session_id, start)) &&
                wait_for(context, SESSION_STARTED, 10000);
    }

    std::vector<uint8_t> audio(640);
    bool interrupt_sent = false;
    bool local_handoff = false;
    bool source_finished = false;
    while (ready && !(is_cancelled && is_cancelled())) {
        if (context.interrupt_requested.load() && !interrupt_sent) {
            interrupt_sent = send_packet(
                client, volc::encode_json_event(515, session_id, "{}"));
            if (!interrupt_sent) {
                ready = false;
                break;
            }
            local_handoff = true;
            break;
        }
        size_t count = 0;
        if (!source(audio.data(), audio.size(), count)) {
            source_finished = true;
            break;
        }
        if (count && !send_packet(client, volc::encode_audio_event(
                                      200, session_id, audio.data(), count))) {
            ready = false;
        }
    }

    if (ready && source_finished && push_to_talk && !local_handoff) {
        ready = send_packet(
            client, volc::encode_json_event(400, session_id, "{}"));
        for (int waited_ms = 0;
             ready && waited_ms < 60000;
             waited_ms += 100) {
            const EventBits_t bits = xEventGroupGetBits(context.events);
            if (bits & FAILED) {
                ready = false;
                break;
            }
            if (bits & TURN_FINISHED) {
                break;
            }
            if (context.interrupt_requested.load()) {
                interrupt_sent = send_packet(
                    client,
                    volc::encode_json_event(515, session_id, "{}"));
                ready = interrupt_sent;
                local_handoff = interrupt_sent;
                break;
            }
            if (is_cancelled && is_cancelled()) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (ready && !local_handoff &&
            !(xEventGroupGetBits(context.events) & TURN_FINISHED) &&
            !(is_cancelled && is_cancelled())) {
            ready = false;
        }
    }

    if (is_cancelled && is_cancelled()) {
        send_packet(client, volc::encode_json_event(515, session_id, "{}"));
        result.cancelled = true;
    }
    if (esp_websocket_client_is_connected(client)) {
        send_packet(client, volc::encode_json_event(102, session_id, "{}"));
        send_packet(client, volc::encode_json_event(2, "", "{}"));
    }
    esp_websocket_client_stop(client);
    esp_websocket_client_destroy(client);

    if (!result.cancelled && ready) {
        result.success = true;
    } else if (!result.cancelled) {
        xSemaphoreTake(context.mutex, portMAX_DELAY);
        result.error_hint = context.error;
        xSemaphoreGive(context.mutex);
        if (result.error_hint.empty()) {
            result.error_hint = "端到端语音会话意外结束";
        }
    }
    vSemaphoreDelete(context.mutex);
    vEventGroupDelete(context.events);
    return result;
}

}  // namespace audio
}  // namespace smart_fridge
