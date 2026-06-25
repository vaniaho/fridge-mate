#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace smart_fridge {
namespace audio {

struct tts_result_t {
    bool success = false;
    bool cancelled = false;
    size_t audio_bytes = 0;
    std::string format;
    std::string error_hint;
};

using tts_audio_callback_t =
    std::function<bool(const uint8_t *pcm, size_t length)>;
using tts_cancel_callback_t = std::function<bool(void)>;

/**
 * Synthesize one complete sentence. PCM chunks are decoded and forwarded from
 * the HTTP receive callback, allowing playback before the response completes.
 */
tts_result_t tts_synthesize_stream(const std::string &text,
                                   const tts_audio_callback_t &on_audio,
                                   const tts_cancel_callback_t &is_cancelled);

}  // namespace audio
}  // namespace smart_fridge
