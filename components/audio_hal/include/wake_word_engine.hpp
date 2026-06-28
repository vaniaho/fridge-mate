#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#if __has_include("esp_wn_iface.h") && __has_include("esp_wn_models.h") && \
    __has_include("model_path.h")
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "model_path.h"
#define SMART_FRIDGE_HAS_ESP_SR_WAKENET 1
#else
#define SMART_FRIDGE_HAS_ESP_SR_WAKENET 0
#endif

namespace smart_fridge {
namespace audio {

struct wake_word_result_t {
    bool detected = false;
    bool engine_ready = false;
    int rms = 0;
    int noise_floor = 0;
    int score = 0;
    int wake_id = 0;
};

class wake_word_engine_t {
public:
    bool init();
    void shutdown();
    void reset();
    wake_word_result_t process_16k_mono(const uint8_t *pcm, size_t bytes);

private:
    bool init_wakenet();
    bool run_wakenet(const uint8_t *pcm, size_t bytes,
                     wake_word_result_t &result);

    int noise_floor_ = 120;
    int voiced_ms_ = 0;
    int quiet_ms_ = 0;
    bool init_attempted_ = false;
    bool ready_ = false;
    std::string model_name_;
    std::vector<int16_t> pending_samples_;

#if SMART_FRIDGE_HAS_ESP_SR_WAKENET
    srmodel_list_t *models_ = nullptr;
    const esp_wn_iface_t *iface_ = nullptr;
    model_iface_data_t *model_data_ = nullptr;
    int samples_per_chunk_ = 0;
#endif
};

}  // namespace audio
}  // namespace smart_fridge
