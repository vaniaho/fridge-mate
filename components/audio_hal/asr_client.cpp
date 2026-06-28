#include "asr_client.hpp"

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
#include "websocket_diagnostics.hpp"
#include "zlib.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

static const char *TAG = "ASRStream";

namespace smart_fridge {
namespace audio {

namespace {

constexpr EventBits_t WS_CONNECTED = BIT0;
constexpr EventBits_t WS_FINAL = BIT1;
constexpr EventBits_t WS_ERROR = BIT2;
constexpr int CONNECT_TIMEOUT_MS = 20000;
constexpr int FINAL_TIMEOUT_MS = 5000;

struct asr_ws_context_t {
    EventGroupHandle_t events = nullptr;
    SemaphoreHandle_t mutex = nullptr;
    std::vector<uint8_t> frame;
    std::string latest_text;
    std::string final_text;
    std::string error;
    asr_text_callback_t on_text;
    std::atomic<bool> closing{false};
};

uint32_t read_be32(const uint8_t *data)
{
    return (static_cast<uint32_t>(data[0]) << 24) |
           (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) |
           static_cast<uint32_t>(data[3]);
}

void append_be32(std::vector<uint8_t> &out, uint32_t value)
{
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
    out.push_back(static_cast<uint8_t>(value & 0xff));
}

bool gzip_compress(const uint8_t *data, size_t length,
                   std::vector<uint8_t> &out)
{
    z_stream stream = {};
    if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8,
                     Z_DEFAULT_STRATEGY) != Z_OK) {
        return false;
    }

    out.resize(compressBound(length) + 32);
    stream.next_in = const_cast<Bytef *>(data);
    stream.avail_in = static_cast<uInt>(length);
    stream.next_out = out.data();
    stream.avail_out = static_cast<uInt>(out.size());

    const int ret = deflate(&stream, Z_FINISH);
    if (ret != Z_STREAM_END) {
        deflateEnd(&stream);
        out.clear();
        return false;
    }

    out.resize(stream.total_out);
    deflateEnd(&stream);
    return true;
}

bool gzip_decompress(const uint8_t *data, size_t length, std::string &out)
{
    z_stream stream = {};
    if (inflateInit2(&stream, 15 + 32) != Z_OK) {
        return false;
    }

    stream.next_in = const_cast<Bytef *>(data);
    stream.avail_in = static_cast<uInt>(length);
    std::array<uint8_t, 2048> buffer;
    out.clear();

    int ret = Z_OK;
    while (ret == Z_OK) {
        stream.next_out = buffer.data();
        stream.avail_out = static_cast<uInt>(buffer.size());
        ret = inflate(&stream, Z_NO_FLUSH);
        const size_t produced = buffer.size() - stream.avail_out;
        out.append(reinterpret_cast<const char *>(buffer.data()), produced);
    }

    inflateEnd(&stream);
    return ret == Z_STREAM_END;
}

std::string make_connect_id()
{
    char id[33];
    for (int i = 0; i < 4; ++i) {
        snprintf(id + i * 8, 9, "%08lx",
                 static_cast<unsigned long>(esp_random()));
    }
    id[32] = '\0';
    return id;
}

std::string extra_value(const char *extra, const char *key)
{
    if (!extra || !extra[0]) {
        return "";
    }

    cJSON *json = cJSON_Parse(extra);
    if (json) {
        cJSON *item = cJSON_GetObjectItem(json, key);
        std::string value =
            cJSON_IsString(item) && item->valuestring ? item->valuestring : "";
        cJSON_Delete(json);
        return trim_config_value(value.c_str());
    }

    const std::string text(extra);
    const std::string prefix = std::string(key) + "=";
    const size_t start = text.find(prefix);
    if (start != std::string::npos) {
        const size_t value_start = start + prefix.size();
        const size_t end = text.find_first_of(";,", value_start);
        return trim_config_value(
            text.substr(value_start, end - value_start).c_str());
    }

    // Backward compatibility: the old UI stored the app id directly in extra.
    return strcmp(key, "appid") == 0
               ? trim_config_value(text.c_str())
               : "";
}

std::string normalize_ws_url(const char *url)
{
    std::string result = url ? url : "";
    if (result.rfind("https://", 0) == 0) {
        result.replace(0, 5, "wss");
    } else if (result.rfind("http://", 0) == 0) {
        result.replace(0, 4, "ws");
    }
    return result;
}

std::vector<uint8_t> make_packet(uint8_t message_type, uint8_t flags,
                                 uint8_t serialization,
                                 const uint8_t *payload, size_t payload_size,
                                 bool compress)
{
    std::vector<uint8_t> body;
    if (compress && payload_size > 0) {
        if (!gzip_compress(payload, payload_size, body)) {
            return {};
        }
    } else if (payload && payload_size > 0) {
        body.assign(payload, payload + payload_size);
    }

    std::vector<uint8_t> packet;
    packet.reserve(8 + body.size());
    packet.push_back(0x11);  // protocol v1, four-byte header
    packet.push_back(static_cast<uint8_t>((message_type << 4) | flags));
    packet.push_back(static_cast<uint8_t>(
        (serialization << 4) | (compress ? 0x01 : 0x00)));
    packet.push_back(0x00);
    append_be32(packet, static_cast<uint32_t>(body.size()));
    packet.insert(packet.end(), body.begin(), body.end());
    return packet;
}

std::vector<uint8_t> make_full_request()
{
    cJSON *root = cJSON_CreateObject();
    cJSON *user = cJSON_CreateObject();
    cJSON *audio = cJSON_CreateObject();
    cJSON *request = cJSON_CreateObject();
    if (!root || !user || !audio || !request) {
        cJSON_Delete(root);
        cJSON_Delete(user);
        cJSON_Delete(audio);
        cJSON_Delete(request);
        return {};
    }

    cJSON_AddStringToObject(user, "uid", "smart-fridge");
    cJSON_AddStringToObject(audio, "format", "pcm");
    cJSON_AddStringToObject(audio, "codec", "raw");
    cJSON_AddNumberToObject(audio, "rate", 16000);
    cJSON_AddNumberToObject(audio, "bits", 16);
    cJSON_AddNumberToObject(audio, "channel", 1);
    cJSON_AddStringToObject(request, "model_name", "bigmodel");
    cJSON_AddBoolToObject(request, "enable_nonstream", true);
    cJSON_AddBoolToObject(request, "enable_itn", true);
    cJSON_AddBoolToObject(request, "enable_punc", true);
    cJSON_AddStringToObject(request, "result_type", "full");
    cJSON_AddBoolToObject(request, "show_utterances", true);
    cJSON_AddNumberToObject(request, "end_window_size", 800);
    cJSON_AddItemToObject(root, "user", user);
    cJSON_AddItemToObject(root, "audio", audio);
    cJSON_AddItemToObject(root, "request", request);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return {};
    }

    const auto packet = make_packet(
        0x1, 0x0, 0x1, reinterpret_cast<const uint8_t *>(json), strlen(json),
        true);
    free(json);
    return packet;
}

bool json_bool(cJSON *object, const char *name)
{
    cJSON *item = cJSON_GetObjectItem(object, name);
    return cJSON_IsTrue(item) ||
           (cJSON_IsNumber(item) && item->valueint != 0);
}

void append_text(std::string &target, const char *text)
{
    if (!text || !text[0]) {
        return;
    }
    target.append(text);
}

bool extract_result_object(cJSON *result, std::string &text,
                           bool &is_final)
{
    if (!cJSON_IsObject(result)) {
        return false;
    }

    cJSON *text_item = cJSON_GetObjectItem(result, "text");
    if (cJSON_IsString(text_item) && text_item->valuestring &&
        text_item->valuestring[0]) {
        text = text_item->valuestring;
    }

    cJSON *utterances = cJSON_GetObjectItem(result, "utterances");
    if (cJSON_IsArray(utterances)) {
        std::string utterance_text;
        cJSON *utterance = nullptr;
        cJSON_ArrayForEach(utterance, utterances) {
            if (!cJSON_IsObject(utterance)) {
                continue;
            }
            cJSON *utterance_value =
                cJSON_GetObjectItem(utterance, "text");
            if (cJSON_IsString(utterance_value) &&
                utterance_value->valuestring) {
                append_text(utterance_text,
                            utterance_value->valuestring);
            }
            is_final = is_final || json_bool(utterance, "definite") ||
                       json_bool(utterance, "is_final") ||
                       json_bool(utterance, "final");
        }
        if (text.empty()) {
            text = utterance_text;
        }
    }

    return !text.empty();
}

bool extract_transcript(cJSON *root, std::string &text, bool &is_final)
{
    if (!root) {
        return false;
    }

    cJSON *result = cJSON_GetObjectItem(root, "result");
    if (extract_result_object(result, text, is_final)) {
        return true;
    }

    if (cJSON_IsArray(result)) {
        std::string combined;
        cJSON *item = nullptr;
        cJSON_ArrayForEach(item, result) {
            std::string item_text;
            if (extract_result_object(item, item_text, is_final)) {
                append_text(combined, item_text.c_str());
            }
        }
        if (!combined.empty()) {
            text = combined;
            return true;
        }
    }

    // Compatibility with responses that expose the complete transcript at
    // the top level. Do not recursively inspect words[]: their "text" fields
    // are individual characters and would overwrite the complete sentence.
    cJSON *text_item = cJSON_GetObjectItem(root, "text");
    if (cJSON_IsString(text_item) && text_item->valuestring &&
        text_item->valuestring[0]) {
        text = text_item->valuestring;
        is_final = is_final || json_bool(root, "definite") ||
                   json_bool(root, "is_final") ||
                   json_bool(root, "final");
        return true;
    }
    return false;
}

void set_error(asr_ws_context_t *ctx, const std::string &error)
{
    if (!ctx || !ctx->mutex) {
        return;
    }
    xSemaphoreTake(ctx->mutex, portMAX_DELAY);
    if (ctx->error.empty()) {
        ctx->error = error;
    }
    xSemaphoreGive(ctx->mutex);
    xEventGroupSetBits(ctx->events, WS_ERROR);
}

void parse_server_packet(asr_ws_context_t *ctx,
                         const std::vector<uint8_t> &packet)
{
    if (!ctx || packet.size() < 8) {
        return;
    }

    const size_t header_size = (packet[0] & 0x0f) * 4;
    const uint8_t message_type = packet[1] >> 4;
    const uint8_t flags = packet[1] & 0x0f;
    const uint8_t compression = packet[2] & 0x0f;
    size_t offset = header_size;

    if (header_size < 4 || offset > packet.size()) {
        set_error(ctx, "ASR 协议头无效");
        return;
    }

    if (message_type == 0x0f) {
        uint32_t code = 0;
        if (offset + 4 <= packet.size()) {
            code = read_be32(packet.data() + offset);
            offset += 4;
        }
        std::string message = "ASR 服务端错误 " + std::to_string(code);
        if (offset + 4 <= packet.size()) {
            const uint32_t size = read_be32(packet.data() + offset);
            offset += 4;
            if (offset + size <= packet.size()) {
                message.append(": ");
                message.append(reinterpret_cast<const char *>(packet.data() +
                                                              offset),
                               size);
            }
        }
        set_error(ctx, message);
        return;
    }

    if (message_type != 0x09) {
        return;
    }

    bool sequence_final = false;
    if ((flags & 0x01) && offset + 4 <= packet.size()) {
        const int32_t sequence =
            static_cast<int32_t>(read_be32(packet.data() + offset));
        offset += 4;
        if (sequence < 0) {
            sequence_final = true;
        }
    }

    if (offset + 4 > packet.size()) {
        return;
    }
    const uint32_t payload_size = read_be32(packet.data() + offset);
    offset += 4;
    if (offset + payload_size > packet.size()) {
        return;
    }

    std::string json;
    if (compression == 0x01) {
        if (!gzip_decompress(packet.data() + offset, payload_size, json)) {
            set_error(ctx, "ASR 响应解压失败");
            return;
        }
    } else {
        json.assign(reinterpret_cast<const char *>(packet.data() + offset),
                    payload_size);
    }

    cJSON *root = cJSON_ParseWithLength(json.c_str(), json.size());
    if (!root) {
        ESP_LOGW(TAG, "Ignoring non-JSON ASR frame, size=%u",
                 static_cast<unsigned>(json.size()));
        return;
    }

    cJSON *code = cJSON_GetObjectItem(root, "code");
    if (cJSON_IsNumber(code) && code->valueint != 0) {
        cJSON *message = cJSON_GetObjectItem(root, "message");
        set_error(ctx, cJSON_IsString(message) && message->valuestring
                           ? message->valuestring
                           : "ASR 服务端返回错误");
        cJSON_Delete(root);
        return;
    }

    std::string transcript;
    bool is_final = sequence_final;
    extract_transcript(root, transcript, is_final);
    cJSON_Delete(root);

    if (transcript.empty()) {
        if (sequence_final) {
            xEventGroupSetBits(ctx->events, WS_FINAL);
        }
        return;
    }

    bool changed = false;
    xSemaphoreTake(ctx->mutex, portMAX_DELAY);
    if (transcript != ctx->latest_text) {
        ctx->latest_text = transcript;
        changed = true;
    }
    if (is_final) {
        ctx->final_text = transcript;
    }
    xSemaphoreGive(ctx->mutex);

    if (changed && ctx->on_text) {
        ctx->on_text(transcript, is_final);
    }
    if (is_final) {
        xEventGroupSetBits(ctx->events, WS_FINAL);
    }
}

void websocket_event_handler(void *handler_args, esp_event_base_t base,
                             int32_t event_id, void *event_data)
{
    (void)base;
    auto *ctx = static_cast<asr_ws_context_t *>(handler_args);
    auto *data = static_cast<esp_websocket_event_data_t *>(event_data);
    if (!ctx) {
        return;
    }

    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            xEventGroupSetBits(ctx->events, WS_CONNECTED);
            break;
        case WEBSOCKET_EVENT_DATA:
            if (!data || !data->data_ptr || data->data_len <= 0) {
                break;
            }
            if (data->payload_offset == 0) {
                ctx->frame.clear();
                if (data->payload_len > 0) {
                    ctx->frame.reserve(data->payload_len);
                }
            }
            ctx->frame.insert(
                ctx->frame.end(),
                reinterpret_cast<const uint8_t *>(data->data_ptr),
                reinterpret_cast<const uint8_t *>(data->data_ptr) +
                    data->data_len);
            if (data->payload_offset + data->data_len >= data->payload_len) {
                parse_server_packet(ctx, ctx->frame);
                ctx->frame.clear();
            }
            break;
        case WEBSOCKET_EVENT_ERROR:
            set_error(ctx, websocket_error_hint("ASR", data));
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
            if (!ctx->closing.load() &&
                (xEventGroupGetBits(ctx->events) & WS_FINAL) == 0) {
                set_error(ctx, websocket_error_hint("ASR", data));
            }
            break;
        default:
            break;
    }
}

bool send_packet(esp_websocket_client_handle_t client,
                 const std::vector<uint8_t> &packet)
{
    if (!client || packet.empty()) {
        return false;
    }
    const int sent = esp_websocket_client_send_bin(
        client, reinterpret_cast<const char *>(packet.data()), packet.size(),
        pdMS_TO_TICKS(2000));
    return sent == static_cast<int>(packet.size());
}

}  // namespace

asr_result_t asr_recognize_stream(const asr_audio_source_t &source,
                                  const asr_text_callback_t &on_text,
                                  const asr_cancel_callback_t &is_cancelled)
{
    asr_result_t result;
    const voice_model_config_t *stored =
        credentials_get_voice_model_config(VOICE_MODEL_ASR);
    if (!stored) {
        result.error_hint = "ASR 配置不存在";
        return result;
    }
    const voice_model_config_t cfg = *stored;

    const std::string provider = trim_config_value(cfg.provider);
    const std::string url = normalize_ws_url(
        trim_config_value(cfg.url).c_str());
    const std::string api_key = trim_config_value(cfg.key);
    if (url.empty() || api_key.empty()) {
        result.error_hint = "ASR URL 或 Access Key 未配置";
        return result;
    }
    if (!provider.empty() && provider != "volcengine" &&
        provider != "doubao") {
        result.error_hint = "当前仅实现火山引擎流式 ASR 协议";
        return result;
    }
    if (!cloud_tls_clock_ready()) {
        result.error_hint =
            "系统时间尚未同步，无法校验 ASR TLS 证书；请确认 NTP 同步成功";
        return result;
    }

    const std::string app_id = extra_value(cfg.extra, "appid");
    const std::string auth_mode = extra_value(cfg.extra, "auth_mode");
    const std::string configured_resource =
        extra_value(cfg.extra, "resource_id");
    const std::string model = trim_config_value(cfg.model);
    const std::string resource_id = !configured_resource.empty()
        ? configured_resource
        : (model.rfind("volc.", 0) == 0
               ? model : "volc.bigasr.sauc.duration");
    const std::string connect_id = make_connect_id();
    std::string headers;
    const bool legacy_auth =
        auth_mode == "legacy" || auth_mode == "access_token";
    if (legacy_auth) {
        if (app_id.empty()) {
            result.error_hint =
                "ASR 旧版鉴权需要在 Extra 中配置 appid";
            return result;
        }
        headers = "X-Api-App-Key: " + app_id + "\r\n" +
                  "X-Api-Access-Key: " + api_key + "\r\n";
    } else {
        // 新控制台 API Key 鉴权不需要 App ID。Extra 中即使保留了
        // appid，也不能据此自动切换成旧版 Access Token 鉴权。
        headers = "X-Api-Key: " + api_key + "\r\n";
    }
    headers += "X-Api-Resource-Id: " + resource_id + "\r\n" +
               "X-Api-Connect-Id: " + connect_id + "\r\n";
    ESP_LOGI(TAG, "Connecting %s (resource=%s, auth=%s)",
             url.c_str(), resource_id.c_str(),
             legacy_auth ? "legacy" : "api-key");
    cloud_tls_log_heap("ASR", "before connect");

    asr_ws_context_t context;
    context.events = xEventGroupCreate();
    context.mutex = xSemaphoreCreateMutex();
    context.on_text = on_text;
    if (!context.events || !context.mutex) {
        if (context.events) {
            vEventGroupDelete(context.events);
        }
        if (context.mutex) {
            vSemaphoreDelete(context.mutex);
        }
        result.error_hint = "ASR 会话内存不足";
        return result;
    }

    esp_websocket_client_config_t ws_cfg = {};
    ws_cfg.uri = url.c_str();
    ws_cfg.headers = headers.c_str();
    ws_cfg.crt_bundle_attach = esp_crt_bundle_attach;
    ws_cfg.network_timeout_ms = CONNECT_TIMEOUT_MS;
    ws_cfg.reconnect_timeout_ms = 3000;
    ws_cfg.disable_auto_reconnect = true;
    ws_cfg.buffer_size = 4096;
    ws_cfg.task_stack = 8192;

    esp_websocket_client_handle_t client =
        esp_websocket_client_init(&ws_cfg);
    if (!client) {
        vSemaphoreDelete(context.mutex);
        vEventGroupDelete(context.events);
        result.error_hint = "无法创建 ASR WebSocket 客户端";
        return result;
    }

    esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY,
                                  websocket_event_handler, &context);
    esp_err_t err = esp_websocket_client_start(client);
    if (err != ESP_OK) {
        result.error_hint =
            "ASR WebSocket 启动失败：" + std::string(esp_err_to_name(err));
        esp_websocket_client_destroy(client);
        cloud_tls_log_heap("ASR", "after start failure");
        vSemaphoreDelete(context.mutex);
        vEventGroupDelete(context.events);
        return result;
    }

    EventBits_t bits = xEventGroupWaitBits(
        context.events, WS_CONNECTED | WS_ERROR, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(CONNECT_TIMEOUT_MS));
    if ((bits & WS_CONNECTED) == 0) {
        xSemaphoreTake(context.mutex, portMAX_DELAY);
        result.error_hint = context.error;
        xSemaphoreGive(context.mutex);
        if (result.error_hint.empty()) {
            result.error_hint =
                "ASR WebSocket 连接超时（20 秒）：请检查设备 DNS 和外网连接";
        }
        context.closing.store(true);
        esp_websocket_client_stop(client);
        esp_websocket_client_destroy(client);
        cloud_tls_log_heap("ASR", "after connect failure");
        vSemaphoreDelete(context.mutex);
        vEventGroupDelete(context.events);
        return result;
    }

    const auto request = make_full_request();
    if (!send_packet(client, request)) {
        result.error_hint = "ASR 会话参数发送失败";
    } else {
        std::array<uint8_t, 6400> pcm;
        bool last_sent = false;
        while (!last_sent && result.error_hint.empty()) {
            if (is_cancelled && is_cancelled()) {
                result.cancelled = true;
                break;
            }

            size_t bytes_read = 0;
            bool is_last = false;
            if (!source(pcm.data(), pcm.size(), bytes_read, is_last)) {
                result.error_hint = "麦克风音频流读取失败";
                break;
            }

            if (bytes_read > 0 || is_last) {
                const auto audio_packet =
                    make_packet(0x2, is_last ? 0x2 : 0x0, 0x0, pcm.data(),
                                bytes_read, true);
                if (!send_packet(client, audio_packet)) {
                    result.error_hint = "ASR 音频帧发送失败";
                    break;
                }
            }
            last_sent = is_last;
        }

        if (last_sent && !result.cancelled && result.error_hint.empty()) {
            bits = xEventGroupWaitBits(context.events, WS_FINAL | WS_ERROR,
                                       pdFALSE, pdFALSE,
                                       pdMS_TO_TICKS(FINAL_TIMEOUT_MS));
            if (bits & WS_FINAL) {
                vTaskDelay(pdMS_TO_TICKS(120));
            }
        }
    }

    context.closing.store(true);
    esp_websocket_client_stop(client);
    esp_websocket_client_destroy(client);
    cloud_tls_log_heap("ASR", "after close");

    xSemaphoreTake(context.mutex, portMAX_DELAY);
    result.text = context.final_text.empty() ? context.latest_text
                                             : context.final_text;
    if (result.error_hint.empty()) {
        result.error_hint = context.error;
    }
    xSemaphoreGive(context.mutex);

    if (!result.cancelled && result.error_hint.empty() && !result.text.empty()) {
        result.success = true;
        if (on_text) {
            on_text(result.text, true);
        }
    } else if (!result.cancelled && result.error_hint.empty()) {
        result.error_hint = "未识别到有效语音";
    }

    vSemaphoreDelete(context.mutex);
    vEventGroupDelete(context.events);
    return result;
}

}  // namespace audio
}  // namespace smart_fridge
