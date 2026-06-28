#include "wake_word_engine.hpp"

#include "esp_log.h"
#include "sdkconfig.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace smart_fridge {
namespace audio {

namespace {

constexpr const char *TAG = "WakeWord";
constexpr int kFrameMs = 20;

int calculate_pcm_rms(const uint8_t *pcm, size_t bytes)
{
    const size_t samples = bytes / sizeof(int16_t);
    if (!pcm || samples == 0) {
        return 0;
    }

    uint64_t sum = 0;
    for (size_t i = 0; i < samples; ++i) {
        int16_t sample = 0;
        memcpy(&sample, pcm + i * sizeof(sample), sizeof(sample));
        const int32_t value = sample;
        sum += static_cast<uint64_t>(value * value);
    }
    return static_cast<int>(sqrt(static_cast<double>(sum) / samples));
}

#if SMART_FRIDGE_HAS_ESP_SR_WAKENET
det_mode_t configured_detection_mode()
{
#if CONFIG_SMART_FRIDGE_WAKE_WORD_DETECTION_MODE <= 90
    return DET_MODE_90;
#else
    return DET_MODE_95;
#endif
}
#endif

}  // namespace

bool wake_word_engine_t::init()
{
    if (init_attempted_) {
        return ready_;
    }
    init_attempted_ = true;
    ready_ = init_wakenet();
    if (!ready_) {
        ESP_LOGW(TAG, "WakeNet unavailable; wake detector backend is inactive");
    }
    return ready_;
}

void wake_word_engine_t::shutdown()
{
#if SMART_FRIDGE_HAS_ESP_SR_WAKENET
    if (iface_ && model_data_) {
        iface_->destroy(model_data_);
    }
    model_data_ = nullptr;
    iface_ = nullptr;
    samples_per_chunk_ = 0;
    pending_samples_.clear();
    if (models_) {
        esp_srmodel_deinit(models_);
        models_ = nullptr;
    }
#endif
    ready_ = false;
    init_attempted_ = false;
}

void wake_word_engine_t::reset()
{
    voiced_ms_ = 0;
    quiet_ms_ = 0;
    pending_samples_.clear();
}

bool wake_word_engine_t::init_wakenet()
{
#if SMART_FRIDGE_HAS_ESP_SR_WAKENET
    models_ = esp_srmodel_init("model");
    if (!models_) {
        ESP_LOGW(TAG, "No ESP-SR model partition named 'model'");
        return false;
    }

    const char *configured = CONFIG_SMART_FRIDGE_WAKE_WORD_MODEL_NAME;
    char *model_name =
        configured && configured[0]
            ? esp_srmodel_filter(models_, ESP_WN_PREFIX, configured)
            : nullptr;
    if (!model_name) {
        model_name = esp_srmodel_filter(models_, ESP_WN_PREFIX, nullptr);
    }
    if (!model_name) {
        ESP_LOGW(TAG, "No WakeNet model found in model partition");
        return false;
    }

    iface_ = esp_wn_handle_from_name(model_name);
    if (!iface_) {
        ESP_LOGW(TAG, "WakeNet interface not found for %s", model_name);
        return false;
    }

    model_data_ = iface_->create(model_name, configured_detection_mode());
    if (!model_data_) {
        ESP_LOGW(TAG, "Failed to create WakeNet model %s", model_name);
        return false;
    }

    samples_per_chunk_ = iface_->get_samp_chunksize(model_data_);
    if (samples_per_chunk_ <= 0) {
        ESP_LOGW(TAG, "Invalid WakeNet chunk size for %s", model_name);
        iface_->destroy(model_data_);
        model_data_ = nullptr;
        return false;
    }

    model_name_ = model_name;
    pending_samples_.reserve(static_cast<size_t>(samples_per_chunk_) * 2);
    ESP_LOGI(TAG, "WakeNet ready: model=%s chunk=%d samples mode=%d",
             model_name_.c_str(), samples_per_chunk_,
             CONFIG_SMART_FRIDGE_WAKE_WORD_DETECTION_MODE);
    return true;
#else
    ESP_LOGW(TAG, "ESP-SR WakeNet headers are not available");
    return false;
#endif
}

bool wake_word_engine_t::run_wakenet(const uint8_t *pcm, size_t bytes,
                                     wake_word_result_t &result)
{
#if SMART_FRIDGE_HAS_ESP_SR_WAKENET
    if (!ready_ || !iface_ || !model_data_ || samples_per_chunk_ <= 0 ||
        !pcm || bytes < sizeof(int16_t)) {
        return false;
    }

    const auto *samples = reinterpret_cast<const int16_t *>(pcm);
    const size_t sample_count = bytes / sizeof(int16_t);
    pending_samples_.insert(pending_samples_.end(), samples,
                            samples + sample_count);

    bool detected = false;
    const size_t chunk_samples = static_cast<size_t>(samples_per_chunk_);
    while (pending_samples_.size() >= chunk_samples) {
        const int wake_id = iface_->detect(model_data_, pending_samples_.data());
        if (wake_id > 0) {
            result.detected = true;
            result.wake_id = wake_id;
            result.score = 100;
            detected = true;
        }
        pending_samples_.erase(pending_samples_.begin(),
                               pending_samples_.begin() + chunk_samples);
    }
    return detected;
#else
    (void)pcm;
    (void)bytes;
    (void)result;
    return false;
#endif
}

wake_word_result_t wake_word_engine_t::process_16k_mono(const uint8_t *pcm,
                                                        size_t bytes)
{
    wake_word_result_t result;
    if (!init_attempted_) {
        init();
    }
    result.engine_ready = ready_;
    result.rms = calculate_pcm_rms(pcm, bytes);

    if (result.rms > 0 && result.rms < noise_floor_ * 2) {
        noise_floor_ = (noise_floor_ * 31 + result.rms) / 32;
        noise_floor_ = std::max(noise_floor_, 80);
    }
    result.noise_floor = noise_floor_;

    if (run_wakenet(pcm, bytes, result)) {
        return result;
    }

#if CONFIG_SMART_FRIDGE_WAKE_WORD_DEBUG_ENERGY_DETECT
    const int threshold = std::max(
        CONFIG_SMART_FRIDGE_WAKE_WORD_RMS_THRESHOLD, noise_floor_ * 4);
    const bool voiced = result.rms >= threshold;
    result.score = threshold > 0 ? std::min(100, result.rms * 100 / threshold)
                                 : 0;

    if (voiced) {
        voiced_ms_ += kFrameMs;
        quiet_ms_ = 0;
    } else {
        quiet_ms_ += kFrameMs;
        if (quiet_ms_ >= 160) {
            voiced_ms_ = 0;
        }
    }

    if (voiced_ms_ >= CONFIG_SMART_FRIDGE_WAKE_WORD_MIN_VOICE_MS) {
        result.detected = true;
        reset();
    }
#else
    result.score = 0;
    result.detected = false;
#endif

    return result;
}

}  // namespace audio
}  // namespace smart_fridge
