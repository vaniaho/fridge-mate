#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace smart_fridge {
namespace audio {

struct asr_result_t {
    bool success = false;
    bool cancelled = false;
    std::string text;
    std::string error_hint;
};

using asr_audio_source_t = std::function<bool(
    uint8_t *buffer, size_t capacity, size_t &bytes_read, bool &is_last)>;
using asr_text_callback_t =
    std::function<void(const std::string &text, bool is_final)>;
using asr_cancel_callback_t = std::function<bool(void)>;

/**
 * Open a WebSocket ASR session and continuously pull 16 kHz/16-bit/mono PCM
 * from source. source returns false on capture failure; is_last marks the final
 * audio frame. Partial and final transcripts are reported through on_text.
 */
asr_result_t asr_recognize_stream(const asr_audio_source_t &source,
                                  const asr_text_callback_t &on_text,
                                  const asr_cancel_callback_t &is_cancelled);

}  // namespace audio
}  // namespace smart_fridge
