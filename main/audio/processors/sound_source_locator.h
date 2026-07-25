#ifndef SOUND_SOURCE_LOCATOR_H
#define SOUND_SOURCE_LOCATOR_H

#include <array>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

#include <esp_afe_doa.h>

class SoundSourceLocator {
public:
    SoundSourceLocator(const char* input_format, int sample_rate,
        float resolution_degrees, float microphone_distance_m,
        int window_samples, float minimum_rms);
    ~SoundSourceLocator();

    SoundSourceLocator(const SoundSourceLocator&) = delete;
    SoundSourceLocator& operator=(const SoundSourceLocator&) = delete;

    bool IsReady() const;
    void Feed(const std::vector<int16_t>& data, bool playback_active);
    void Reset();
    void OnDirection(std::function<void(float)> callback);

private:
    static constexpr int kInputChannels = 3;
    static constexpr int64_t kLogIntervalUs = 250000;

    afe_doa_handle_t* doa_ = nullptr;
    int window_samples_;
    float minimum_rms_;
    std::vector<int16_t> input_buffer_;
    std::array<float, 3> angle_history_ = {};
    size_t angle_history_count_ = 0;
    size_t angle_history_position_ = 0;
    float smoothed_angle_ = 0.0f;
    bool has_smoothed_angle_ = false;
    bool playback_active_ = false;
    bool invalid_input_logged_ = false;
    int64_t last_log_time_us_ = 0;
    std::mutex mutex_;
    std::function<void(float)> direction_callback_;

    bool HasEnoughEnergy(const int16_t* frame) const;
    float ConvertAndSmooth(float raw_angle);
};

#endif  // SOUND_SOURCE_LOCATOR_H
