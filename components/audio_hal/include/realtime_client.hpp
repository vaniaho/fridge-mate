#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace smart_fridge {
namespace audio {

using realtime_audio_source_t = std::function<bool(
    uint8_t *buffer, size_t capacity, size_t &bytes_read)>;
using realtime_cancel_callback_t = std::function<bool(void)>;

struct realtime_callbacks_t {
    // Return true for a final transcript that should interrupt native model
    // output because the device will route it through local business tools.
    std::function<bool(const std::string &, bool)> on_asr;
    std::function<void(const std::string &)> on_chat_text;
    std::function<bool(const uint8_t *, size_t, int)> on_audio;
    std::function<void(void)> on_barge_in;
    std::function<void(void)> on_turn_finished;
    std::function<void(const std::string &)> on_error;
};

struct realtime_result_t {
    bool success = false;
    bool cancelled = false;
    std::string error_hint;
};

realtime_result_t realtime_dialogue_run(
    const realtime_audio_source_t &audio_source,
    const realtime_callbacks_t &callbacks,
    const realtime_cancel_callback_t &is_cancelled,
    bool push_to_talk = false);

}  // namespace audio
}  // namespace smart_fridge
