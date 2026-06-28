#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AUDIO_EVT_WAKE_WORD,
    AUDIO_EVT_LISTENING_START,
    AUDIO_EVT_LISTENING_STOP,
    AUDIO_EVT_ASR_PARTIAL,
    AUDIO_EVT_ASR_RESULT,
    AUDIO_EVT_ASR_ERROR,
    AUDIO_EVT_TTS_START,
    AUDIO_EVT_TTS_DONE,
    AUDIO_EVT_TTS_INTERRUPTED,
    AUDIO_EVT_TTS_ERROR,
    AUDIO_EVT_REALTIME_TEXT,
    AUDIO_EVT_REALTIME_TURN_DONE,
} audio_hal_event_t;

typedef void (*audio_hal_event_cb_t)(audio_hal_event_t evt, const char *data);
typedef void (*audio_hal_pcm_output_cb_t)(const uint8_t *pcm, size_t length,
                                          int sample_rate);

typedef enum {
    AUDIO_VOICE_MODE_CASCADE = 0,
    AUDIO_VOICE_MODE_REALTIME = 1,
} audio_voice_mode_t;

/**
 * Initialize ES7210/ES8311, the shared I2S bus, and capture/playback rings.
 */
esp_err_t audio_hal_init(void);

void audio_hal_set_event_callback(audio_hal_event_cb_t cb);
void audio_hal_set_pcm_output_callback(audio_hal_pcm_output_cb_t cb);

/**
 * Start/stop the local wake-word task. The task owns microphone polling while
 * idle, keeps listening during normal TTS for barge-in, and pauses during
 * ASR/realtime sessions. The production WakeNet/TFLM keyword backend can be
 * swapped behind the detector adapter.
 */
esp_err_t audio_hal_start_wake_word(void);
esp_err_t audio_hal_stop_wake_word(void);
esp_err_t audio_hal_configure_wake_word(bool enabled, int sensitivity,
                                        bool tts_barge_in_enabled);

/**
 * Stream real microphone PCM to cloud ASR. Local endpoint detection closes
 * the utterance after speech followed by silence.
 */
esp_err_t audio_hal_start_listening(void);
esp_err_t audio_hal_stop_listening(void);
esp_err_t audio_hal_set_next_listening_timeout_ms(int timeout_ms);

/** Browser/external 16 kHz mono PCM input. */
esp_err_t audio_hal_start_external_listening(void);
esp_err_t audio_hal_push_external_pcm(const uint8_t *pcm, size_t length);
esp_err_t audio_hal_finish_external_listening(void);

/**
 * Synthesize one complete response and stream it to the speaker.
 */
esp_err_t audio_hal_play_tts(const char *text);
esp_err_t audio_hal_tts_begin_response(void);
esp_err_t audio_hal_tts_enqueue_sentence(const char *text);
esp_err_t audio_hal_tts_end_response(void);
esp_err_t audio_hal_stop_tts(void);
esp_err_t audio_hal_interrupt(void);

esp_err_t audio_hal_set_voice_mode(audio_voice_mode_t mode);
audio_voice_mode_t audio_hal_get_voice_mode(void);

/**
 * Apply the persisted 0..100 volume to the real speaker codec.
 */
esp_err_t audio_hal_set_output_volume(int percent);

bool audio_hal_is_listening(void);
bool audio_hal_is_playing_tts(void);

#ifdef __cplusplus
}
#endif
