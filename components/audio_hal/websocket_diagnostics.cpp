#include "websocket_diagnostics.hpp"

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace smart_fridge {
namespace audio {

namespace {

int parse_named_int(const std::string &text, const char *name,
                    int fallback = 0)
{
    const std::string marker = std::string(name) + "=";
    const size_t start = text.find(marker);
    if (start == std::string::npos) return fallback;
    const char *value = text.c_str() + start + marker.size();
    char *end = nullptr;
    const long parsed = strtol(value, &end, 10);
    return end == value ? fallback : static_cast<int>(parsed);
}

std::string status_hint(const std::string &prefix, int status)
{
    if (status == 400) {
        return prefix +
               " WebSocket 握手被拒绝（HTTP 400）：请检查接口地址、"
               "鉴权方式和资源 ID";
    }
    if (status == 401) {
        return prefix +
               " 鉴权失败（HTTP 401）：API Key 无效、已过期或填入了"
               "旧版 Access Token";
    }
    if (status == 403) {
        return prefix +
               " 无权访问（HTTP 403）：请确认当前 API Key 所属项目已开通"
               "对应 ASR/TTS 资源";
    }
    if (status == 404) {
        return prefix + " 接口不存在（HTTP 404）：请恢复默认 WebSocket URL";
    }
    if (status == 429) {
        return prefix + " 请求被限流（HTTP 429）：请检查配额和并发限制";
    }
    if (status > 0) {
        return prefix + " WebSocket 握手失败（HTTP " +
               std::to_string(status) + "）";
    }
    return "";
}

}  // namespace

std::string trim_config_value(const char *value)
{
    std::string text = value ? value : "";
    size_t start = 0;
    while (start < text.size() &&
           std::isspace(static_cast<unsigned char>(text[start]))) {
        ++start;
    }
    size_t end = text.size();
    while (end > start &&
           std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return text.substr(start, end - start);
}

bool cloud_tls_clock_ready()
{
    std::time_t now = 0;
    std::time(&now);
    std::tm time_info = {};
    localtime_r(&now, &time_info);
    return time_info.tm_year >= (2024 - 1900);
}

void cloud_tls_log_heap(const char *service, const char *stage)
{
    ESP_LOGI("CloudTLS",
             "%s %s heap: internal=%u, largest_internal=%u, psram=%u",
             service ? service : "cloud",
             stage ? stage : "",
             static_cast<unsigned>(heap_caps_get_free_size(
                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(
                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(
                 heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
}

std::string websocket_error_hint(
    const char *service, const esp_websocket_event_data_t *data)
{
    const std::string prefix = service ? service : "WebSocket";
    if (!data) {
        return prefix + " WebSocket 连接失败（未收到诊断信息）";
    }

    // esp-websocket-client includes the reliable pre-close transport details
    // in ERROR event data_ptr. Prefer it over optional event struct fields.
    const std::string raw =
        data->data_ptr && data->data_len > 0
            ? std::string(data->data_ptr, data->data_len)
            : "";
    if (!raw.empty()) {
        const int raw_status =
            parse_named_int(raw, "esp_ws_handshake_status_code");
        const std::string http_hint = status_hint(prefix, raw_status);
        if (!http_hint.empty()) return http_hint;

        if (raw.find("getaddrinfo") != std::string::npos ||
            raw.find("DNS") != std::string::npos) {
            return prefix + " DNS 解析失败，请检查设备网络和 DNS";
        }

        const int tls_error = parse_named_int(raw, "tls_error_code");
        const int tls_flags = parse_named_int(raw, "tls_flags");
        if (tls_flags > 0 && tls_flags <= 0x1ffff) {
            char buffer[160];
            std::time_t now = 0;
            std::time(&now);
            std::tm time_info = {};
            localtime_r(&now, &time_info);
            snprintf(buffer, sizeof(buffer),
                     "%s TLS 证书校验失败（flags=0x%x，设备时间=%04d-%02d-%02d）",
                     prefix.c_str(), tls_flags, time_info.tm_year + 1900,
                     time_info.tm_mon + 1, time_info.tm_mday);
            return buffer;
        }
        if (tls_error != 0 ||
            raw.find("SSL_HANDSHAKE_FAILED") != std::string::npos) {
            const unsigned int tls_code =
                tls_error < 0
                    ? static_cast<unsigned int>(-tls_error)
                    : static_cast<unsigned int>(tls_error);
            char buffer[128];
            if (tls_code) {
                snprintf(buffer, sizeof(buffer),
                         "%s TLS 握手失败（mbedTLS=-0x%x）",
                         prefix.c_str(), tls_code);
            } else {
                snprintf(buffer, sizeof(buffer), "%s TLS 握手失败",
                         prefix.c_str());
            }
            return buffer;
        }

        const int socket_errno = parse_named_int(raw, "errno");
        if ((socket_errno == EALREADY || socket_errno == EINPROGRESS) &&
            raw.find("esp_transport_connect() failed") != std::string::npos &&
            tls_error == 0 && tls_flags == 0 && raw_status == 0) {
            // esp_transport_ws can leave the non-blocking connect errno behind
            // when TLS has already succeeded but the HTTP Upgrade step fails.
            // Reporting it as a socket connection race hides the real stage.
            return prefix +
                   " WebSocket HTTP 升级握手失败（TLS 已建立但未收到完整响应；"
                   "请检查接口地址和握手响应头容量）";
        }
        if (socket_errno != 0) {
            return prefix + " 网络套接字错误：" +
                   std::string(strerror(socket_errno));
        }
    }

    const auto &error = data->error_handle;
    const int status = error.esp_ws_handshake_status_code;
    const std::string http_hint = status_hint(prefix, status);
    if (!http_hint.empty()) return http_hint;

    // Do not consume optional TLS fields here. esp-websocket-client 1.7.0
    // leaves them undefined in early ERROR events. Reliable transport details
    // are parsed from data_ptr above; HTTP status remains safe to use here.
    if (error.error_type == WEBSOCKET_ERROR_TYPE_HANDSHAKE) {
        return prefix + " WebSocket 握手失败";
    }
    if (error.error_type == WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT) {
        return prefix + " 无法建立 TLS/TCP 连接，请检查 DNS、网络和系统时间";
    }
    return prefix + " WebSocket 连接错误";
}

}  // namespace audio
}  // namespace smart_fridge
