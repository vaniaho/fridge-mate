#include "audio_api.h"

#include "asr_client.hpp"
#include "bsp/esp-bsp.h"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"
#include "realtime_client.hpp"
#include "system_events.h"
#include "tts_client.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

static const char *TAG = "AudioHAL";

namespace smart_fridge {
namespace audio {

namespace {

constexpr uint32_t SAMPLE_RATE = 16000;
constexpr uint8_t BITS_PER_SAMPLE = 16;
constexpr uint8_t HARDWARE_CHANNELS = 2;
constexpr size_t BYTES_PER_SAMPLE = BITS_PER_SAMPLE / 8;
constexpr size_t HARDWARE_FRAME_BYTES = 1280;  // 20 ms stereo
constexpr size_t MONO_FRAME_BYTES = HARDWARE_FRAME_BYTES / HARDWARE_CHANNELS;
constexpr size_t CAPTURE_RING_SIZE = 64 * 1024;
constexpr size_t EXTERNAL_CAPTURE_RING_SIZE = 64 * 1024;
constexpr size_t PLAYBACK_RING_SIZE = 64 * 1024;
constexpr float MICROPHONE_GAIN_DB = 24.0f;
constexpr int DEFAULT_OUTPUT_VOLUME = 60;
constexpr int FRAME_DURATION_MS = 20;
constexpr int SPEECH_RMS_THRESHOLD = 500;
constexpr int END_SILENCE_MS = 800;
constexpr int NO_SPEECH_TIMEOUT_MS = 5000;
constexpr int MAX_UTTERANCE_MS = 15000;
constexpr size_t TTS_QUEUE_LENGTH = 12;
constexpr size_t ASR_PACKET_BYTES = 6400;  // 200 ms mono PCM

enum class input_source_t : uint8_t {
    HARDWARE,
    EXTERNAL,
};

enum class tts_item_type_t : uint8_t {
    SENTENCE,
    END,
};

struct tts_queue_item_t {
    tts_item_type_t type;
    uint32_t generation;
    char *text;
};

static bool s_initialized = false;
static std::atomic<audio_voice_mode_t> s_voice_mode{
    AUDIO_VOICE_MODE_CASCADE};
static std::atomic<bool> s_listening{false};
static std::atomic<bool> s_capture_running{false};
static std::atomic<bool> s_asr_running{false};
static std::atomic<bool> s_asr_finish_requested{false};
static std::atomic<bool> s_asr_cancel_requested{false};
static std::atomic<input_source_t> s_input_source{input_source_t::HARDWARE};
static std::atomic<bool> s_external_input_done{false};
static std::atomic<bool> s_realtime_running{false};
static std::atomic<bool> s_realtime_playback{false};
static std::atomic<bool> s_realtime_turn_done{false};
static std::array<uint8_t, 6> s_realtime_resample_group = {};
static size_t s_realtime_resample_bytes = 0;

static std::atomic<bool> s_playing_tts{false};
static std::atomic<bool> s_tts_accepting{false};
static std::atomic<bool> s_tts_stop_requested{false};
static std::atomic<bool> s_playback_input_done{false};
static std::atomic<bool> s_playback_error{false};
static std::atomic<bool> s_playback_done_notified{false};
static std::atomic<uint32_t> s_tts_generation{0};

static audio_hal_event_cb_t s_event_cb = nullptr;
static audio_hal_pcm_output_cb_t s_pcm_output_cb = nullptr;
static esp_codec_dev_handle_t s_speaker = nullptr;
static esp_codec_dev_handle_t s_microphone = nullptr;
static RingbufHandle_t s_capture_ring = nullptr;
static RingbufHandle_t s_external_capture_ring = nullptr;
static RingbufHandle_t s_playback_ring = nullptr;
static QueueHandle_t s_tts_queue = nullptr;
static int s_output_volume = DEFAULT_OUTPUT_VOLUME;

static TaskHandle_t s_wake_word_task = nullptr;
static TaskHandle_t s_capture_task = nullptr;
static TaskHandle_t s_asr_task = nullptr;
static TaskHandle_t s_realtime_task = nullptr;
static TaskHandle_t s_tts_worker_task = nullptr;
static TaskHandle_t s_playback_task = nullptr;

void emit_event(audio_hal_event_t event, const char *data)
{
    if (s_event_cb) {
        s_event_cb(event, data);
    }
}

void dispatch_voice_command(const std::string &text)
{
    auto *payload =
        static_cast<voice_cmd_payload_t *>(malloc(sizeof(voice_cmd_payload_t)));
    if (!payload) {
        ESP_LOGE(TAG, "Failed to allocate voice command payload");
        return;
    }

    memset(payload, 0, sizeof(*payload));
    strncpy(payload->command_text, text.c_str(),
            sizeof(payload->command_text) - 1);
    if (send_system_event(EVT_VOICE_CMD_RCVD, payload, sizeof(*payload)) !=
        ESP_OK) {
        free(payload);
        ESP_LOGE(TAG, "Failed to dispatch voice command");
    }
}

RingbufHandle_t create_audio_ring(size_t size)
{
    RingbufHandle_t ring = xRingbufferCreateWithCaps(
        size, RINGBUF_TYPE_NOSPLIT, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ring) {
        ring = xRingbufferCreate(size, RINGBUF_TYPE_NOSPLIT);
    }
    return ring;
}

void drain_ring(RingbufHandle_t ring)
{
    if (!ring) {
        return;
    }
    size_t size = 0;
    while (void *item = xRingbufferReceive(ring, &size, 0)) {
        vRingbufferReturnItem(ring, item);
    }
}

void drain_tts_queue()
{
    if (!s_tts_queue) {
        return;
    }
    tts_queue_item_t item = {};
    while (xQueueReceive(s_tts_queue, &item, 0) == pdTRUE) {
        free(item.text);
    }
}

bool send_playback_item(const void *data, size_t size)
{
    while (!s_tts_stop_requested.load() && !s_playback_error.load()) {
        if (xRingbufferSend(s_playback_ring, data, size,
                            pdMS_TO_TICKS(100)) == pdTRUE) {
            return true;
        }
    }
    return false;
}

void stereo_to_mono_left(const uint8_t *stereo, size_t stereo_size,
                         uint8_t *mono, size_t mono_capacity,
                         size_t *mono_written)
{
    size_t output = 0;
    for (size_t input = 0;
         input + HARDWARE_CHANNELS * BYTES_PER_SAMPLE <= stereo_size &&
         output + BYTES_PER_SAMPLE <= mono_capacity;
         input += HARDWARE_CHANNELS * BYTES_PER_SAMPLE) {
        memcpy(mono + output, stereo + input, BYTES_PER_SAMPLE);
        output += BYTES_PER_SAMPLE;
    }
    *mono_written = output;
}

size_t mono_to_stereo(const uint8_t *mono, size_t mono_size, uint8_t *stereo,
                      size_t stereo_capacity)
{
    const size_t samples =
        std::min(mono_size / BYTES_PER_SAMPLE,
                 stereo_capacity /
                     (HARDWARE_CHANNELS * BYTES_PER_SAMPLE));
    for (size_t i = 0; i < samples; ++i) {
        int16_t sample = 0;
        memcpy(&sample, mono + i * BYTES_PER_SAMPLE, sizeof(sample));
        memcpy(stereo + (i * HARDWARE_CHANNELS) * BYTES_PER_SAMPLE, &sample,
               sizeof(sample));
        memcpy(stereo + (i * HARDWARE_CHANNELS + 1) * BYTES_PER_SAMPLE,
               &sample, sizeof(sample));
    }
    return samples * HARDWARE_CHANNELS * BYTES_PER_SAMPLE;
}

int calculate_rms(const uint8_t *pcm, size_t size)
{
    const size_t sample_count = size / sizeof(int16_t);
    if (sample_count == 0) {
        return 0;
    }
    uint64_t sum = 0;
    for (size_t i = 0; i < sample_count; ++i) {
        int16_t sample = 0;
        memcpy(&sample, pcm + i * sizeof(sample), sizeof(sample));
        const int32_t value = sample;
        sum += static_cast<uint64_t>(value * value);
    }
    return static_cast<int>(sqrt(static_cast<double>(sum) / sample_count));
}

bool read_mono_input_frame(uint8_t *buffer, size_t capacity,
                           size_t &bytes_read, TickType_t timeout)
{
    bytes_read = 0;
    const input_source_t source = s_input_source.load();
    RingbufHandle_t ring = source == input_source_t::EXTERNAL
                               ? s_external_capture_ring
                               : s_capture_ring;
    size_t item_size = 0;
    auto *item = static_cast<uint8_t *>(
        xRingbufferReceive(ring, &item_size, timeout));
    if (!item) {
        return false;
    }

    if (source == input_source_t::EXTERNAL) {
        bytes_read = std::min(item_size, capacity);
        memcpy(buffer, item, bytes_read);
    } else {
        stereo_to_mono_left(item, item_size, buffer, capacity,
                            &bytes_read);
    }
    vRingbufferReturnItem(ring, item);
    return bytes_read > 0;
}

bool is_local_business_command(const std::string &text)
{
    static const char *keywords[] = {
        "放入", "放进", "存入", "添加", "拿出", "取出", "删除",
        "库存", "冰箱里", "有什么", "临期", "过期", "保质期",
        "吃什么", "推荐菜", "食谱", "确认", "取消",
    };
    for (const char *keyword : keywords) {
        if (text.find(keyword) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool write_realtime_pcm_24k(const uint8_t *data, size_t length)
{
    if (!data || length == 0) {
        return true;
    }
    std::array<uint8_t, MONO_FRAME_BYTES> mono16 = {};
    size_t output = 0;
    std::array<uint8_t, HARDWARE_FRAME_BYTES> stereo = {};
    auto flush = [&]() {
        if (output == 0) {
            return true;
        }
        if (s_pcm_output_cb) {
            s_pcm_output_cb(mono16.data(), output, 16000);
        }
        const size_t stereo_bytes = mono_to_stereo(
            mono16.data(), output, stereo.data(), stereo.size());
        output = 0;
        if (stereo_bytes == 0) {
            return false;
        }
        s_realtime_playback.store(true);
        s_realtime_turn_done.store(false);
        s_playing_tts.store(true);
        return send_playback_item(stereo.data(), stereo_bytes);
    };

    // 24 kHz -> 16 kHz: retain two samples from every three. The six-byte
    // group is kept across WebSocket frames so packet boundaries cannot
    // truncate audio or shift the resampling phase.
    for (size_t offset = 0; offset < length; ++offset) {
        s_realtime_resample_group[s_realtime_resample_bytes++] =
            data[offset];
        if (s_realtime_resample_bytes !=
            s_realtime_resample_group.size()) {
            continue;
        }
        memcpy(mono16.data() + output,
               s_realtime_resample_group.data(), 2);
        memcpy(mono16.data() + output + 2,
               s_realtime_resample_group.data() + 4, 2);
        output += 4;
        s_realtime_resample_bytes = 0;
        if (output == mono16.size() && !flush()) {
            return false;
        }
    }
    return flush();
}

class pcm_playback_writer_t {
public:
    bool write(const uint8_t *data, size_t length)
    {
        if (s_pcm_output_cb && data && length) {
            s_pcm_output_cb(data, length, SAMPLE_RATE);
        }
        while (length > 0) {
            const size_t copy =
                std::min(length, mono_buffer_.size() - buffered_);
            memcpy(mono_buffer_.data() + buffered_, data, copy);
            buffered_ += copy;
            data += copy;
            length -= copy;
            if (buffered_ == mono_buffer_.size() && !flush_frame()) {
                return false;
            }
        }
        return true;
    }

    bool finish()
    {
        buffered_ -= buffered_ % BYTES_PER_SAMPLE;
        return buffered_ == 0 || flush_frame();
    }

private:
    bool flush_frame()
    {
        const size_t stereo_bytes =
            mono_to_stereo(mono_buffer_.data(), buffered_,
                           stereo_buffer_.data(), stereo_buffer_.size());
        buffered_ = 0;
        return stereo_bytes > 0 &&
               send_playback_item(stereo_buffer_.data(), stereo_bytes);
    }

    std::array<uint8_t, MONO_FRAME_BYTES> mono_buffer_ = {};
    std::array<uint8_t, HARDWARE_FRAME_BYTES> stereo_buffer_ = {};
    size_t buffered_ = 0;
};

static void wake_word_task(void *parameters)
{
    (void)parameters;
    ESP_LOGI(TAG, "Wake-word consumer ready (detector adapter pending)");
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void capture_task(void *parameters)
{
    (void)parameters;
    std::array<uint8_t, HARDWARE_FRAME_BYTES> frame;
    uint32_t dropped_frames = 0;

    while (s_capture_running.load()) {
        const int ret =
            esp_codec_dev_read(s_microphone, frame.data(), frame.size());
        if (ret != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "Microphone read failed: 0x%x", ret);
            s_capture_running.store(false);
            break;
        }
        if (xRingbufferSend(s_capture_ring, frame.data(), frame.size(),
                            pdMS_TO_TICKS(20)) != pdTRUE) {
            ++dropped_frames;
            if ((dropped_frames % 50) == 1) {
                ESP_LOGW(TAG, "Capture ring full, dropped %lu frames",
                         static_cast<unsigned long>(dropped_frames));
            }
        }
    }

    s_capture_task = nullptr;
    vTaskDelete(nullptr);
}

static void streaming_asr_task(void *parameters)
{
    (void)parameters;
    bool speech_started = false;
    int total_ms = 0;
    int silence_ms = 0;

    const asr_audio_source_t source =
        [&](uint8_t *buffer, size_t capacity, size_t &bytes_read,
            bool &is_last) -> bool {
        bytes_read = 0;
        is_last = false;

        while (!s_asr_cancel_requested.load() && bytes_read < capacity) {
            size_t frame_bytes = 0;
            if (!read_mono_input_frame(buffer + bytes_read,
                                       capacity - bytes_read, frame_bytes,
                                       pdMS_TO_TICKS(200))) {
                const bool input_ended =
                    s_input_source.load() == input_source_t::EXTERNAL
                        ? s_external_input_done.load()
                        : !s_capture_running.load();
                if (input_ended) {
                    is_last = true;
                    break;
                }
                continue;
            }

            total_ms += FRAME_DURATION_MS;
            const int rms =
                calculate_rms(buffer + bytes_read, frame_bytes);
            if (rms >= SPEECH_RMS_THRESHOLD) {
                speech_started = true;
                silence_ms = 0;
            } else if (speech_started) {
                silence_ms += FRAME_DURATION_MS;
            }
            bytes_read += frame_bytes;

            is_last = s_asr_finish_requested.load() ||
                      (speech_started && silence_ms >= END_SILENCE_MS) ||
                      (!speech_started &&
                       total_ms >= NO_SPEECH_TIMEOUT_MS) ||
                      total_ms >= MAX_UTTERANCE_MS;
            if (is_last) {
                s_capture_running.store(false);
                s_listening.store(false);
                break;
            }
        }
        return bytes_read > 0 || is_last;
    };

    const asr_text_callback_t on_text =
        [](const std::string &text, bool is_final) {
            if (!is_final) {
                emit_event(AUDIO_EVT_ASR_PARTIAL, text.c_str());
            }
        };

    ESP_LOGI(TAG, "Streaming ASR session started");
    asr_result_t result = asr_recognize_stream(
        source, on_text,
        []() { return s_asr_cancel_requested.load(); });

    s_capture_running.store(false);
    s_listening.store(false);
    emit_event(AUDIO_EVT_LISTENING_STOP, nullptr);

    if (result.success) {
        emit_event(AUDIO_EVT_ASR_RESULT, result.text.c_str());
        dispatch_voice_command(result.text);
    } else if (!result.cancelled) {
        ESP_LOGE(TAG, "ASR failed: %s", result.error_hint.c_str());
        emit_event(AUDIO_EVT_ASR_ERROR, result.error_hint.c_str());
    }

    s_asr_running.store(false);
    s_asr_task = nullptr;
    vTaskDelete(nullptr);
}

static void realtime_voice_task(void *parameters)
{
    (void)parameters;
    std::string realtime_reply;
    std::string local_command;
    realtime_callbacks_t callbacks;
    callbacks.on_asr = [&](const std::string &text, bool final) {
        emit_event(final ? AUDIO_EVT_ASR_RESULT : AUDIO_EVT_ASR_PARTIAL,
                   text.c_str());
        if (final && is_local_business_command(text)) {
            local_command = text;
            return true;
        }
        return false;
    };
    callbacks.on_chat_text = [&](const std::string &text) {
        realtime_reply.append(text);
        emit_event(AUDIO_EVT_REALTIME_TEXT, text.c_str());
    };
    callbacks.on_audio = [](const uint8_t *pcm, size_t length,
                            int sample_rate) {
        if (sample_rate != 24000) {
            return false;
        }
        return write_realtime_pcm_24k(pcm, length);
    };
    callbacks.on_barge_in = []() {
        drain_ring(s_playback_ring);
        s_realtime_resample_bytes = 0;
        s_realtime_playback.store(false);
        s_realtime_turn_done.store(false);
        s_playing_tts.store(false);
        emit_event(AUDIO_EVT_TTS_INTERRUPTED, nullptr);
    };
    callbacks.on_turn_finished = [&]() {
        s_realtime_turn_done.store(true);
        emit_event(AUDIO_EVT_REALTIME_TURN_DONE,
                   realtime_reply.empty() ? nullptr
                                          : realtime_reply.c_str());
        realtime_reply.clear();
    };
    callbacks.on_error = [](const std::string &error) {
        emit_event(AUDIO_EVT_ASR_ERROR, error.c_str());
    };

    realtime_result_t result = realtime_dialogue_run(
        [](uint8_t *buffer, size_t capacity, size_t &bytes_read) {
            bytes_read = 0;
            while (!s_asr_cancel_requested.load()) {
                if (read_mono_input_frame(buffer, capacity, bytes_read,
                                          pdMS_TO_TICKS(200))) {
                    return true;
                }
                const bool input_ended =
                    s_input_source.load() == input_source_t::EXTERNAL
                        ? s_external_input_done.load()
                        : !s_capture_running.load();
                if (input_ended) {
                    return false;
                }
            }
            return false;
        },
        callbacks, []() { return s_asr_cancel_requested.load(); },
        s_input_source.load() == input_source_t::EXTERNAL);

    if (!result.success && !result.cancelled &&
        !result.error_hint.empty()) {
        emit_event(AUDIO_EVT_ASR_ERROR, result.error_hint.c_str());
    }
    s_capture_running.store(false);
    s_listening.store(false);
    s_realtime_running.store(false);
    s_realtime_playback.store(false);
    s_playing_tts.store(false);
    emit_event(AUDIO_EVT_LISTENING_STOP, nullptr);
    if (!local_command.empty() && result.success) {
        dispatch_voice_command(local_command);
    }
    s_realtime_task = nullptr;
    vTaskDelete(nullptr);
}

static void playback_task(void *parameters)
{
    (void)parameters;
    while (true) {
        if (!s_playing_tts.load()) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        size_t item_size = 0;
        auto *item = static_cast<uint8_t *>(xRingbufferReceive(
            s_playback_ring, &item_size, pdMS_TO_TICKS(50)));
        if (item) {
            if (!s_tts_stop_requested.load()) {
                const int ret = esp_codec_dev_write(
                    s_speaker, item, static_cast<int>(item_size));
                if (ret != ESP_CODEC_DEV_OK) {
                    ESP_LOGE(TAG, "Speaker write failed: 0x%x", ret);
                    s_playback_error.store(true);
                }
            }
            vRingbufferReturnItem(s_playback_ring, item);
            continue;
        }

        if ((s_playback_input_done.load() ||
             s_tts_stop_requested.load() || s_playback_error.load()) &&
            !s_realtime_playback.load() &&
            !s_playback_done_notified.exchange(true)) {
            if (s_tts_worker_task) {
                xTaskNotifyGive(s_tts_worker_task);
            }
        }
        if (s_realtime_playback.load() &&
            s_realtime_turn_done.load()) {
            s_realtime_playback.store(false);
            s_realtime_turn_done.store(false);
            s_playing_tts.store(false);
        }
    }
}

void finish_tts_session(bool error)
{
    const bool interrupted = s_tts_stop_requested.load();
    drain_ring(s_playback_ring);
    s_tts_accepting.store(false);
    s_playing_tts.store(false);
    s_playback_input_done.store(false);
    s_playback_done_notified.store(false);

    if (error) {
        emit_event(AUDIO_EVT_TTS_ERROR, "语音合成或播放失败");
    } else if (interrupted) {
        emit_event(AUDIO_EVT_TTS_INTERRUPTED, nullptr);
    } else {
        emit_event(AUDIO_EVT_TTS_DONE, nullptr);
        send_system_event(EVT_TTS_PLAY_DONE, nullptr, 0);
    }
}

static void tts_worker_task(void *parameters)
{
    (void)parameters;
    tts_queue_item_t item = {};
    while (true) {
        if (xQueueReceive(s_tts_queue, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        const uint32_t generation = s_tts_generation.load();
        if (item.generation != generation || !s_playing_tts.load()) {
            free(item.text);
            continue;
        }

        if (item.type == tts_item_type_t::END) {
            free(item.text);
            s_tts_accepting.store(false);
            s_playback_input_done.store(true);
            if (!s_playback_done_notified.load()) {
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(30000));
            }
            finish_tts_session(s_playback_error.load());
            continue;
        }

        std::string sentence = item.text ? item.text : "";
        free(item.text);
        if (sentence.empty() || s_tts_stop_requested.load()) {
            continue;
        }

        emit_event(AUDIO_EVT_TTS_START, sentence.c_str());
        pcm_playback_writer_t writer;
        const tts_result_t result = tts_synthesize_stream(
            sentence,
            [&](const uint8_t *pcm, size_t length) {
                return !s_tts_stop_requested.load() &&
                       writer.write(pcm, length);
            },
            []() { return s_tts_stop_requested.load(); });

        if (!result.cancelled && result.success && !writer.finish()) {
            s_playback_error.store(true);
        } else if (!result.cancelled && !result.success) {
            ESP_LOGE(TAG, "TTS sentence failed: %s",
                     result.error_hint.c_str());
            s_playback_error.store(true);
        }

        if (s_playback_error.load()) {
            drain_tts_queue();
            s_playback_input_done.store(true);
            if (!s_playback_done_notified.load()) {
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));
            }
            finish_tts_session(true);
        }
    }
}

esp_err_t initialize_codec()
{
    s_speaker = bsp_audio_codec_speaker_init();
    s_microphone = bsp_audio_codec_microphone_init();
    if (!s_speaker || !s_microphone) {
        ESP_LOGE(TAG, "Failed to initialize ES8311/ES7210");
        return ESP_FAIL;
    }

    esp_codec_dev_sample_info_t format = {};
    format.sample_rate = SAMPLE_RATE;
    format.channel = HARDWARE_CHANNELS;
    format.bits_per_sample = BITS_PER_SAMPLE;

    int ret = esp_codec_dev_set_out_vol(s_speaker, s_output_volume);
    if (ret != ESP_CODEC_DEV_OK) {
        return ESP_FAIL;
    }
    ret = esp_codec_dev_set_in_gain(s_microphone, MICROPHONE_GAIN_DB);
    if (ret != ESP_CODEC_DEV_OK) {
        return ESP_FAIL;
    }
    ret = esp_codec_dev_open(s_speaker, &format);
    if (ret != ESP_CODEC_DEV_OK) {
        return ESP_FAIL;
    }
    ret = esp_codec_dev_open(s_microphone, &format);
    if (ret != ESP_CODEC_DEV_OK) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

bool enqueue_tts_item(tts_item_type_t type, const char *text)
{
    tts_queue_item_t item = {};
    item.type = type;
    item.generation = s_tts_generation.load();
    item.text = text ? strdup(text) : nullptr;
    if (text && !item.text) {
        return false;
    }
    if (xQueueSend(s_tts_queue, &item, pdMS_TO_TICKS(200)) != pdTRUE) {
        free(item.text);
        return false;
    }
    return true;
}

esp_err_t start_input_session(input_source_t source)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_listening.load()) {
        return ESP_OK;
    }
    if (s_asr_running.load() || s_realtime_running.load()) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_playing_tts.load() && !s_realtime_playback.load()) {
        audio_hal_stop_tts();
    }
    s_input_source.store(source);
    s_external_input_done.store(false);
    drain_ring(source == input_source_t::EXTERNAL
                   ? s_external_capture_ring
                   : s_capture_ring);
    s_asr_finish_requested.store(false);
    s_asr_cancel_requested.store(false);
    s_listening.store(true);
    s_capture_running.store(source == input_source_t::HARDWARE);
    emit_event(AUDIO_EVT_LISTENING_START, nullptr);

    if (source == input_source_t::HARDWARE &&
        xTaskCreate(capture_task, "audio_capture", 4096, nullptr, 6,
                    &s_capture_task) != pdPASS) {
        s_capture_running.store(false);
        s_listening.store(false);
        return ESP_FAIL;
    }

    if (s_voice_mode.load() == AUDIO_VOICE_MODE_REALTIME) {
        s_tts_stop_requested.store(false);
        s_playback_error.store(false);
        s_realtime_resample_bytes = 0;
        s_realtime_running.store(true);
        if (xTaskCreate(realtime_voice_task, "voice_realtime", 28672,
                        nullptr, 5, &s_realtime_task) != pdPASS) {
            s_realtime_running.store(false);
            s_capture_running.store(false);
            s_listening.store(false);
            return ESP_FAIL;
        }
    } else {
        s_asr_running.store(true);
        if (xTaskCreate(streaming_asr_task, "asr_stream", 24576, nullptr,
                        5, &s_asr_task) != pdPASS) {
            s_asr_running.store(false);
            s_capture_running.store(false);
            s_listening.store(false);
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

}  // namespace

}  // namespace audio
}  // namespace smart_fridge

extern "C" {

esp_err_t audio_hal_init(void)
{
    using namespace smart_fridge::audio;
    if (s_initialized) {
        return ESP_OK;
    }

    s_capture_ring = create_audio_ring(CAPTURE_RING_SIZE);
    s_external_capture_ring =
        create_audio_ring(EXTERNAL_CAPTURE_RING_SIZE);
    s_playback_ring = create_audio_ring(PLAYBACK_RING_SIZE);
    s_tts_queue = xQueueCreate(TTS_QUEUE_LENGTH, sizeof(tts_queue_item_t));
    if (!s_capture_ring || !s_external_capture_ring ||
        !s_playback_ring || !s_tts_queue) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = initialize_codec();
    if (err != ESP_OK) {
        return err;
    }

    if (xTaskCreate(playback_task, "audio_playback", 4096, nullptr, 6,
                    &s_playback_task) != pdPASS ||
        xTaskCreate(tts_worker_task, "tts_stream", 24576, nullptr, 4,
                    &s_tts_worker_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Audio streaming HAL initialized");
    return ESP_OK;
}

void audio_hal_set_event_callback(audio_hal_event_cb_t callback)
{
    smart_fridge::audio::s_event_cb = callback;
}

void audio_hal_set_pcm_output_callback(audio_hal_pcm_output_cb_t callback)
{
    smart_fridge::audio::s_pcm_output_cb = callback;
}

esp_err_t audio_hal_start_wake_word(void)
{
    using namespace smart_fridge::audio;
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_wake_word_task) {
        return ESP_OK;
    }
    return xTaskCreate(wake_word_task, "wake_word", 4096, nullptr, 3,
                       &s_wake_word_task) == pdPASS
               ? ESP_OK
               : ESP_FAIL;
}

esp_err_t audio_hal_stop_wake_word(void)
{
    using namespace smart_fridge::audio;
    if (s_wake_word_task) {
        vTaskDelete(s_wake_word_task);
        s_wake_word_task = nullptr;
    }
    return ESP_OK;
}

esp_err_t audio_hal_start_listening(void)
{
    using namespace smart_fridge::audio;
    return start_input_session(input_source_t::HARDWARE);
}

esp_err_t audio_hal_stop_listening(void)
{
    using namespace smart_fridge::audio;
    if (s_listening.load()) {
        s_asr_finish_requested.store(true);
        if (s_input_source.load() == input_source_t::EXTERNAL) {
            s_external_input_done.store(true);
        }
    }
    return ESP_OK;
}

esp_err_t audio_hal_start_external_listening(void)
{
    using namespace smart_fridge::audio;
    return start_input_session(input_source_t::EXTERNAL);
}

esp_err_t audio_hal_push_external_pcm(const uint8_t *pcm, size_t length)
{
    using namespace smart_fridge::audio;
    if (!pcm || length == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_listening.load() ||
        s_input_source.load() != input_source_t::EXTERNAL) {
        return ESP_ERR_INVALID_STATE;
    }
    return xRingbufferSend(s_external_capture_ring, pcm, length,
                           pdMS_TO_TICKS(20)) == pdTRUE
               ? ESP_OK
               : ESP_ERR_TIMEOUT;
}

esp_err_t audio_hal_finish_external_listening(void)
{
    using namespace smart_fridge::audio;
    s_external_input_done.store(true);
    s_asr_finish_requested.store(true);
    return ESP_OK;
}

esp_err_t audio_hal_tts_begin_response(void)
{
    using namespace smart_fridge::audio;
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_playing_tts.load()) {
        audio_hal_stop_tts();
        for (int retry = 0; retry < 100 && s_playing_tts.load(); ++retry) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (s_playing_tts.load()) {
            return ESP_ERR_TIMEOUT;
        }
    }

    drain_tts_queue();
    drain_ring(s_playback_ring);
    s_tts_generation.fetch_add(1);
    s_tts_stop_requested.store(false);
    s_playback_input_done.store(false);
    s_playback_error.store(false);
    s_playback_done_notified.store(false);
    s_tts_accepting.store(true);
    s_playing_tts.store(true);
    return ESP_OK;
}

esp_err_t audio_hal_tts_enqueue_sentence(const char *text)
{
    using namespace smart_fridge::audio;
    if (!text || !text[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_playing_tts.load() || !s_tts_accepting.load()) {
        return ESP_ERR_INVALID_STATE;
    }
    return enqueue_tts_item(tts_item_type_t::SENTENCE, text) ? ESP_OK
                                                              : ESP_FAIL;
}

esp_err_t audio_hal_tts_end_response(void)
{
    using namespace smart_fridge::audio;
    if (!s_playing_tts.load() || !s_tts_accepting.exchange(false)) {
        return ESP_ERR_INVALID_STATE;
    }
    return enqueue_tts_item(tts_item_type_t::END, nullptr) ? ESP_OK
                                                            : ESP_FAIL;
}

esp_err_t audio_hal_play_tts(const char *text)
{
    if (!text || !text[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = audio_hal_tts_begin_response();
    if (err != ESP_OK) {
        return err;
    }
    err = audio_hal_tts_enqueue_sentence(text);
    if (err != ESP_OK) {
        audio_hal_stop_tts();
        return err;
    }
    return audio_hal_tts_end_response();
}

esp_err_t audio_hal_stop_tts(void)
{
    using namespace smart_fridge::audio;
    if (!s_playing_tts.load()) {
        return ESP_OK;
    }
    s_tts_accepting.store(false);
    s_tts_stop_requested.store(true);
    s_playback_input_done.store(true);
    drain_ring(s_playback_ring);
    drain_tts_queue();
    enqueue_tts_item(tts_item_type_t::END, nullptr);
    return ESP_OK;
}

esp_err_t audio_hal_interrupt(void)
{
    using namespace smart_fridge::audio;
    s_asr_cancel_requested.store(true);
    s_asr_finish_requested.store(false);
    s_external_input_done.store(true);
    s_capture_running.store(false);
    s_listening.store(false);
    s_realtime_playback.store(false);
    s_realtime_turn_done.store(false);
    s_realtime_resample_bytes = 0;
    drain_ring(s_playback_ring);
    const esp_err_t tts_result = audio_hal_stop_tts();
    for (int retry = 0;
         retry < 200 &&
         (s_asr_running.load() || s_realtime_running.load());
         ++retry) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return tts_result;
}

esp_err_t audio_hal_set_voice_mode(audio_voice_mode_t mode)
{
    using namespace smart_fridge::audio;
    if (mode != AUDIO_VOICE_MODE_CASCADE &&
        mode != AUDIO_VOICE_MODE_REALTIME) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_listening.load() || s_asr_running.load() ||
        s_realtime_running.load() ||
        (s_playing_tts.load() && !s_tts_stop_requested.load())) {
        return ESP_ERR_INVALID_STATE;
    }
    s_voice_mode.store(mode);
    return ESP_OK;
}

audio_voice_mode_t audio_hal_get_voice_mode(void)
{
    return smart_fridge::audio::s_voice_mode.load();
}

esp_err_t audio_hal_set_output_volume(int percent)
{
    using namespace smart_fridge::audio;
    if (percent < 0 || percent > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    s_output_volume = percent;
    if (!s_initialized || !s_speaker) {
        return ESP_OK;
    }
    return esp_codec_dev_set_out_vol(s_speaker, percent) == ESP_CODEC_DEV_OK
               ? ESP_OK
               : ESP_FAIL;
}

bool audio_hal_is_listening(void)
{
    return smart_fridge::audio::s_listening.load();
}

bool audio_hal_is_playing_tts(void)
{
    return smart_fridge::audio::s_playing_tts.load();
}

}  // extern "C"
