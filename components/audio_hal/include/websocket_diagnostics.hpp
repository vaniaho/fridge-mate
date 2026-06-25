#pragma once

#include "esp_websocket_client.h"

#include <string>

namespace smart_fridge {
namespace audio {

std::string trim_config_value(const char *value);
bool cloud_tls_clock_ready();
void cloud_tls_log_heap(const char *service, const char *stage);
std::string websocket_error_hint(
    const char *service, const esp_websocket_event_data_t *data);

}  // namespace audio
}  // namespace smart_fridge
