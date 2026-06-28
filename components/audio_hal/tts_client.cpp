#include "tts_client.hpp"

#include "cJSON.h"
#include "credentials_manager.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "volc_binary_protocol.hpp"
#include "websocket_diagnostics.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>
#include <vector>

namespace smart_fridge {
namespace audio {

namespace {

constexpr EventBits_t TRANSPORT_CONNECTED = BIT0;
constexpr EventBits_t CONNECTION_STARTED = BIT1;
constexpr EventBits_t SESSION_STARTED = BIT2;
constexpr EventBits_t SENTENCE_FINISHED = BIT3;
constexpr EventBits_t SESSION_FINISHED = BIT4;
constexpr EventBits_t SESSION_CANCELLED = BIT5;
constexpr EventBits_t FAILED = BIT6;
constexpr EventBits_t SYNTHESIS_STARTED = BIT7;
constexpr int CONNECT_TIMEOUT_MS = 20000;
constexpr int SYNTHESIS_TIMEOUT_MS = 30000;
constexpr int SESSION_FINISH_TIMEOUT_MS = 3000;
constexpr int AUDIO_IDLE_COMPLETE_MS = 900;
static std::atomic<bool> s_tts_request_active{false};

struct tts_options_t {
    std::string resource_id = "seed-tts-2.0";
    std::string model = "seed-tts-2.0-standard";
    int sample_rate = 16000;
    int speech_rate = 0;
    int loudness_rate = 0;
};

struct ws_context_t {
    EventGroupHandle_t events = nullptr;
    SemaphoreHandle_t mutex = nullptr;
    std::vector<uint8_t> frame;
    tts_audio_callback_t on_audio;
    tts_cancel_callback_t is_cancelled;
    std::string error;
    std::atomic<size_t> audio_bytes{0};
    std::atomic<bool> callback_failed{false};
    std::atomic<bool> transport_connected{false};
    std::atomic<bool> closing{false};
    std::atomic<bool> finish_requested{false};
    std::atomic<uint32_t> last_event{0};
    std::atomic<int32_t> last_sequence{0};
    std::atomic<TickType_t> last_audio_tick{0};
};

class tts_request_guard_t {
public:
    tts_request_guard_t()
    {
        bool expected = false;
        acquired_ = s_tts_request_active.compare_exchange_strong(
            expected, true);
    }

    ~tts_request_guard_t()
    {
        if (acquired_) {
            s_tts_request_active.store(false);
        }
    }

    bool acquired() const { return acquired_; }

private:
    bool acquired_ = false;
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

tts_options_t parse_options(const char *extra)
{
    tts_options_t options;
    if (!extra || !extra[0]) {
        return options;
    }
    cJSON *root = cJSON_Parse(extra);
    if (!root) {
        return options;
    }
    cJSON *item = cJSON_GetObjectItem(root, "resource_id");
    if (cJSON_IsString(item) && item->valuestring) {
        options.resource_id = trim_config_value(item->valuestring);
    }
    item = cJSON_GetObjectItem(root, "model");
    if (cJSON_IsString(item) && item->valuestring) {
        options.model = trim_config_value(item->valuestring);
    }
    item = cJSON_GetObjectItem(root, "sample_rate");
    if (cJSON_IsNumber(item)) {
        options.sample_rate = item->valueint;
    }
    item = cJSON_GetObjectItem(root, "speech_rate");
    if (cJSON_IsNumber(item)) {
        options.speech_rate = item->valueint;
    }
    item = cJSON_GetObjectItem(root, "loudness_rate");
    if (cJSON_IsNumber(item)) {
        options.loudness_rate = item->valueint;
    }
    cJSON_Delete(root);
    return options;
}

std::string build_start_session_json(const std::string &speaker,
                                     const tts_options_t &options)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *parameters = cJSON_CreateObject();
    cJSON *audio = cJSON_CreateObject();
    if (!root || !parameters || !audio) {
        cJSON_Delete(root);
        cJSON_Delete(parameters);
        cJSON_Delete(audio);
        return "";
    }

    cJSON_AddStringToObject(parameters, "model", options.model.c_str());
    cJSON_AddStringToObject(parameters, "speaker", speaker.c_str());
    cJSON_AddStringToObject(audio, "format", "pcm");
    cJSON_AddNumberToObject(audio, "sample_rate", options.sample_rate);
    cJSON_AddNumberToObject(audio, "speech_rate", options.speech_rate);
    cJSON_AddNumberToObject(audio, "loudness_rate",
                           options.loudness_rate);
    cJSON_AddItemToObject(parameters, "audio_params", audio);
    cJSON_AddItemToObject(root, "req_params", parameters);

    char *json = cJSON_PrintUnformatted(root);
    std::string output = json ? json : "";
    free(json);
    cJSON_Delete(root);
    return output;
}

std::string build_task_json(const std::string &text)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *parameters = cJSON_CreateObject();
    if (!root || !parameters) {
        cJSON_Delete(root);
        cJSON_Delete(parameters);
        return "";
    }
    cJSON_AddStringToObject(parameters, "text", text.c_str());
    cJSON_AddItemToObject(root, "req_params", parameters);
    char *json = cJSON_PrintUnformatted(root);
    std::string output = json ? json : "";
    free(json);
    cJSON_Delete(root);
    return output;
}

bool send_packet(esp_websocket_client_handle_t client,
                 const std::vector<uint8_t> &packet)
{
    if (!client || packet.empty()) {
        return false;
    }
    return esp_websocket_client_send_bin(
               client, reinterpret_cast<const char *>(packet.data()),
               packet.size(), pdMS_TO_TICKS(2000)) ==
           static_cast<int>(packet.size());
}

void set_error(ws_context_t *context, const std::string &error)
{
    if (!context) {
        return;
    }
    if (context->mutex) {
        xSemaphoreTake(context->mutex, portMAX_DELAY);
        if (context->error.empty()) {
            context->error = error;
        }
        xSemaphoreGive(context->mutex);
    }
    xEventGroupSetBits(context->events, FAILED);
}

std::string payload_error(const volc::packet_t &packet)
{
    if (packet.payload.empty()) {
        return "TTS 服务端错误 " + std::to_string(packet.error_code);
    }
    std::string payload(
        reinterpret_cast<const char *>(packet.payload.data()),
        packet.payload.size());
    cJSON *root = cJSON_Parse(payload.c_str());
    if (!root) {
        return payload;
    }
    cJSON *item = cJSON_GetObjectItem(root, "error");
    if (!cJSON_IsString(item)) {
        item = cJSON_GetObjectItem(root, "message");
    }
    std::string error =
        cJSON_IsString(item) && item->valuestring ? item->valuestring
                                                 : payload;
    cJSON_Delete(root);
    return error;
}

void process_packet(ws_context_t *context,
                    const std::vector<uint8_t> &frame)
{
    volc::packet_t packet;
    std::string decode_error;
    if (!volc::decode_packet(frame.data(), frame.size(), packet,
                             decode_error)) {
        set_error(context, decode_error);
        return;
    }
    if (packet.type == volc::message_type_t::ERROR) {
        set_error(context, payload_error(packet));
        return;
    }

    context->last_event.store(packet.event);
    context->last_sequence.store(packet.sequence);
    ESP_LOGD("TTSWebSocket",
             "RX type=0x%x flags=0x%x event=%lu sequence=%ld payload=%u",
             static_cast<unsigned>(packet.type),
             static_cast<unsigned>(packet.flags),
             static_cast<unsigned long>(packet.event),
             static_cast<long>(packet.sequence),
             static_cast<unsigned>(packet.payload.size()));

    const bool audio_packet =
        packet.type == volc::message_type_t::AUDIO_SERVER ||
        packet.event == 352;
    if (audio_packet && !packet.payload.empty()) {
        const size_t previous_audio =
            context->audio_bytes.load();
        if (context->is_cancelled && context->is_cancelled()) {
            context->callback_failed.store(true);
            xEventGroupSetBits(context->events, FAILED);
            return;
        }
        if (context->on_audio &&
            !context->on_audio(packet.payload.data(),
                               packet.payload.size())) {
            context->callback_failed.store(true);
            xEventGroupSetBits(context->events, FAILED);
            return;
        }
        context->audio_bytes.fetch_add(packet.payload.size());
        context->last_audio_tick.store(xTaskGetTickCount());
        if (previous_audio == 0) {
            ESP_LOGI("TTSWebSocket",
                     "First audio received: %u bytes",
                     static_cast<unsigned>(packet.payload.size()));
        }
        xEventGroupSetBits(context->events, SYNTHESIS_STARTED);
    }

    // AudioOnlyServer responses may use a negative sequence or the last-packet
    // flag instead of a separate TTSSentenceEnd event.
    if (packet.type == volc::message_type_t::AUDIO_SERVER &&
        (packet.sequence < 0 || (packet.flags & 0x02) != 0)) {
        xEventGroupSetBits(context->events, SENTENCE_FINISHED);
    }

    switch (packet.event) {
        case 50:
            ESP_LOGI("TTSWebSocket", "Connection started");
            xEventGroupSetBits(context->events, CONNECTION_STARTED);
            break;
        case 51:
        case 153:
            set_error(context, payload_error(packet));
            break;
        case 150:
            ESP_LOGI("TTSWebSocket", "Session started");
            xEventGroupSetBits(context->events, SESSION_STARTED);
            break;
        case 151:
            xEventGroupSetBits(context->events, SESSION_CANCELLED);
            if (!context->is_cancelled || !context->is_cancelled()) {
                set_error(context, "TTS 会话被服务端取消");
            }
            break;
        case 152:
            ESP_LOGI("TTSWebSocket",
                     "Session finished, audio=%u bytes",
                     static_cast<unsigned>(
                         context->audio_bytes.load()));
            xEventGroupSetBits(context->events, SESSION_FINISHED);
            if (!context->finish_requested.load() &&
                (xEventGroupGetBits(context->events) &
                 SENTENCE_FINISHED) == 0) {
                set_error(context,
                          "TTS 服务端在音频合成完成前结束了会话");
            }
            break;
        case 350:
            ESP_LOGI("TTSWebSocket", "Sentence started");
            xEventGroupSetBits(context->events, SYNTHESIS_STARTED);
            break;
        case 351:
            ESP_LOGI("TTSWebSocket",
                     "Sentence finished, audio=%u bytes",
                     static_cast<unsigned>(
                         context->audio_bytes.load()));
            xEventGroupSetBits(context->events, SENTENCE_FINISHED);
            break;
        case 359:
            ESP_LOGI("TTSWebSocket",
                     "TTS ended, audio=%u bytes",
                     static_cast<unsigned>(
                         context->audio_bytes.load()));
            xEventGroupSetBits(context->events, SENTENCE_FINISHED);
            break;
        case 352:
            break;
        default:
            break;
    }
}

void websocket_event_handler(void *handler_args, esp_event_base_t,
                             int32_t event_id, void *event_data)
{
    auto *context = static_cast<ws_context_t *>(handler_args);
    auto *data = static_cast<esp_websocket_event_data_t *>(event_data);
    if (!context) {
        return;
    }

    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            context->transport_connected.store(true);
            ESP_LOGI("TTSWebSocket", "WebSocket connected");
            xEventGroupSetBits(context->events, TRANSPORT_CONNECTED);
            break;
        case WEBSOCKET_EVENT_DATA:
            if (!data || !data->data_ptr || data->data_len <= 0) {
                break;
            }
            if (data->payload_offset == 0) {
                context->frame.clear();
                context->frame.reserve(data->payload_len);
            }
            context->frame.insert(
                context->frame.end(),
                reinterpret_cast<const uint8_t *>(data->data_ptr),
                reinterpret_cast<const uint8_t *>(data->data_ptr) +
                    data->data_len);
            if (data->payload_offset + data->data_len >=
                data->payload_len) {
                process_packet(context, context->frame);
                context->frame.clear();
            }
            break;
        case WEBSOCKET_EVENT_ERROR:
            set_error(context, websocket_error_hint("TTS", data));
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
            context->transport_connected.store(false);
            if (!context->closing.load() &&
                (xEventGroupGetBits(context->events) &
                 (SENTENCE_FINISHED | SESSION_FINISHED)) == 0) {
                set_error(context, websocket_error_hint("TTS", data));
            }
            break;
        default:
            break;
    }
}

bool wait_for(ws_context_t &context, EventBits_t success, int timeout_ms)
{
    const EventBits_t bits = xEventGroupWaitBits(
        context.events, success | FAILED, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(timeout_ms));
    return (bits & success) != 0 && (bits & FAILED) == 0;
}

bool wait_for_synthesis(ws_context_t &context)
{
    const TickType_t start = xTaskGetTickCount();
    const TickType_t timeout = pdMS_TO_TICKS(SYNTHESIS_TIMEOUT_MS);
    const TickType_t idle =
        pdMS_TO_TICKS(AUDIO_IDLE_COMPLETE_MS);
    while ((xTaskGetTickCount() - start) < timeout) {
        const EventBits_t bits = xEventGroupGetBits(context.events);
        if (bits & FAILED) return false;
        if (bits & SENTENCE_FINISHED) return true;

        const TickType_t last_audio =
            context.last_audio_tick.load();
        if (context.audio_bytes.load() > 0 && last_audio != 0 &&
            (xTaskGetTickCount() - last_audio) >= idle) {
            ESP_LOGI("TTSWebSocket",
                     "Audio stream idle for %d ms; treating sentence as complete (%u bytes)",
                     AUDIO_IDLE_COMPLETE_MS,
                     static_cast<unsigned>(
                         context.audio_bytes.load()));
            xEventGroupSetBits(context.events, SENTENCE_FINISHED);
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return false;
}

}  // namespace

tts_result_t tts_synthesize_stream(const std::string &text,
                                   const tts_audio_callback_t &on_audio,
                                   const tts_cancel_callback_t &is_cancelled)
{
    tts_result_t result;
    tts_request_guard_t request_guard;
    if (!request_guard.acquired()) {
        result.error_hint = "已有 TTS 合成任务正在进行，请稍后重试";
        return result;
    }

    const voice_model_config_t *stored =
        credentials_get_voice_model_config(VOICE_MODEL_TTS);
    if (!stored || text.empty()) {
        result.error_hint = "TTS 配置或文本为空";
        return result;
    }
    const voice_model_config_t config = *stored;
    std::string url = trim_config_value(config.url);
    if (url.rfind("https://", 0) == 0) {
        url.replace(0, 5, "wss");
    } else if (url.rfind("http://", 0) == 0) {
        url.replace(0, 4, "ws");
    }
    const std::string api_key = trim_config_value(config.key);
    const std::string speaker = trim_config_value(config.model);
    if (url.empty() || api_key.empty() || speaker.empty()) {
        result.error_hint = "TTS URL、API Key 或音色未配置";
        return result;
    }
    if (!cloud_tls_clock_ready()) {
        result.error_hint =
            "系统时间尚未同步，无法校验 TTS TLS 证书；请确认 NTP 同步成功";
        return result;
    }

    const tts_options_t options = parse_options(config.extra);
    if (options.sample_rate != 16000) {
        result.error_hint = "板端 TTS 输出采样率必须配置为 16000 Hz";
        return result;
    }
    const std::string connect_id = random_id();
    const std::string session_id = random_id();
    const std::string headers =
        "X-Api-Key: " + api_key + "\r\n" +
        "X-Api-Resource-Id: " + options.resource_id + "\r\n" +
        "X-Api-Connect-Id: " + connect_id + "\r\n";
    ESP_LOGI("TTSWebSocket", "Connecting %s (resource=%s, speaker=%s)",
             url.c_str(), options.resource_id.c_str(), speaker.c_str());
    cloud_tls_log_heap("TTS", "before connect");

    ws_context_t context;
    context.events = xEventGroupCreate();
    context.mutex = xSemaphoreCreateMutex();
    context.on_audio = on_audio;
    context.is_cancelled = is_cancelled;
    if (!context.events || !context.mutex) {
        result.error_hint = "TTS 会话内存不足";
        if (context.events) vEventGroupDelete(context.events);
        if (context.mutex) vSemaphoreDelete(context.mutex);
        return result;
    }

    esp_websocket_client_config_t ws_config = {};
    ws_config.uri = url.c_str();
    ws_config.headers = headers.c_str();
    ws_config.crt_bundle_attach = esp_crt_bundle_attach;
    ws_config.network_timeout_ms = CONNECT_TIMEOUT_MS;
    ws_config.disable_auto_reconnect = true;
    ws_config.buffer_size = 4096;
    ws_config.task_stack = 8192;

    esp_websocket_client_handle_t client =
        esp_websocket_client_init(&ws_config);
    if (!client) {
        result.error_hint = "无法创建 TTS WebSocket";
        vSemaphoreDelete(context.mutex);
        vEventGroupDelete(context.events);
        return result;
    }
    esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY,
                                  websocket_event_handler, &context);

    const esp_err_t start_err = esp_websocket_client_start(client);
    bool started = start_err == ESP_OK;
    if (!started) {
        set_error(&context, "TTS WebSocket 启动失败：" +
                                std::string(esp_err_to_name(start_err)));
    }
    if (started) {
        started = wait_for(context, TRANSPORT_CONNECTED,
                           CONNECT_TIMEOUT_MS);
        if (!started &&
            (xEventGroupGetBits(context.events) & FAILED) == 0) {
            set_error(&context,
                      "TTS WebSocket 连接超时（20 秒）：请检查设备 DNS 和外网连接");
        }
    }
    if (started) {
        started = send_packet(
            client, volc::encode_json_event(1, "", "{}")) &&
                  wait_for(context, CONNECTION_STARTED,
                           CONNECT_TIMEOUT_MS);
        if (!started &&
            (xEventGroupGetBits(context.events) & FAILED) == 0) {
            set_error(&context, "TTS 服务连接初始化超时");
        }
    }
    if (started) {
        const std::string session_json =
            build_start_session_json(speaker, options);
        started = !session_json.empty() &&
                  send_packet(client, volc::encode_json_event(
                                          100, session_id, session_json)) &&
                  wait_for(context, SESSION_STARTED,
                           CONNECT_TIMEOUT_MS);
        if (!started &&
            (xEventGroupGetBits(context.events) & FAILED) == 0) {
            set_error(&context,
                      "TTS 会话创建超时：请检查音色、模型和资源权限");
        }
    }
    if (started) {
        const std::string task_json = build_task_json(text);
        started = !task_json.empty() &&
                  send_packet(client, volc::encode_json_event(
                                          200, session_id, task_json)) &&
                  wait_for_synthesis(context);
        if (!started &&
            (xEventGroupGetBits(context.events) & FAILED) == 0) {
            char timeout_error[160];
            snprintf(timeout_error, sizeof(timeout_error),
                     "TTS 合成超时（已接收 %u 字节音频，最后事件=%lu，序号=%ld）",
                     static_cast<unsigned>(context.audio_bytes.load()),
                     static_cast<unsigned long>(
                         context.last_event.load()),
                     static_cast<long>(context.last_sequence.load()));
            set_error(&context, timeout_error);
        }
    }

    if (is_cancelled && is_cancelled()) {
        send_packet(client,
                    volc::encode_json_event(101, session_id, "{}"));
        result.cancelled = true;
    } else if (started &&
               esp_websocket_client_is_connected(client)) {
        context.finish_requested.store(true);
        if (send_packet(client, volc::encode_json_event(
                                    102, session_id, "{}"))) {
            wait_for(context, SESSION_FINISHED,
                     SESSION_FINISH_TIMEOUT_MS);
        }
    }
    if (esp_websocket_client_is_connected(client)) {
        send_packet(client, volc::encode_json_event(2, "", "{}"));
    }
    context.closing.store(true);
    esp_websocket_client_stop(client);
    esp_websocket_client_destroy(client);
    cloud_tls_log_heap("TTS", "after close");

    result.audio_bytes = context.audio_bytes.load();
    result.format = "pcm";
    if (!result.cancelled && started &&
        !context.callback_failed.load() &&
        context.audio_bytes.load() > 0) {
        result.success = true;
    } else if (!result.cancelled) {
        xSemaphoreTake(context.mutex, portMAX_DELAY);
        result.error_hint = context.error;
        xSemaphoreGive(context.mutex);
        if (result.error_hint.empty()) {
            result.error_hint = context.callback_failed.load()
                                    ? "TTS 音频输出被中止"
                                    : "TTS 未返回有效音频";
        }
    }

    vSemaphoreDelete(context.mutex);
    vEventGroupDelete(context.events);
    return result;
}

}  // namespace audio
}  // namespace smart_fridge
