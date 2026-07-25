#include "sound_source_locator.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include <esp_log.h>
#include <esp_timer.h>

#define TAG "SoundSourceLocator"

SoundSourceLocator::SoundSourceLocator(
    const char* input_format, int sample_rate, float resolution_degrees,
    float microphone_distance_m, int window_samples, float minimum_rms)
    : window_samples_(window_samples),
      minimum_rms_(minimum_rms) {
    input_buffer_.reserve((window_samples_ + 160) * kInputChannels);
    doa_ = afe_doa_create(input_format, sample_rate, resolution_degrees,
        microphone_distance_m, window_samples_);
    if (doa_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create AFE DOA processor");
    }
}

SoundSourceLocator::~SoundSourceLocator() {
    if (doa_ != nullptr) {
        afe_doa_destroy(doa_);
    }
}

bool SoundSourceLocator::IsReady() const {
    return doa_ != nullptr;
}

void SoundSourceLocator::Feed(
    const std::vector<int16_t>& data, bool playback_active) {
    std::function<void(float)> callback;
    float reported_angle = 0.0f;
    bool has_report = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (doa_ == nullptr) {
            return;
        }
        if (playback_active) {
            if (!playback_active_) {
                input_buffer_.clear();
                playback_active_ = true;
            }
            return;
        }
        if (playback_active_) {
            input_buffer_.clear();
            playback_active_ = false;
        }
        if (data.empty()) {
            return;
        }
        if (data.size() % kInputChannels != 0) {
            if (!invalid_input_logged_) {
                ESP_LOGE(TAG, "Expected MMR input, got %u interleaved samples",
                    static_cast<unsigned>(data.size()));
                invalid_input_logged_ = true;
            }
            return;
        }

        input_buffer_.insert(input_buffer_.end(), data.begin(), data.end());
        const size_t frame_size =
            static_cast<size_t>(window_samples_) * kInputChannels;
        while (input_buffer_.size() >= frame_size) {
            const int16_t* frame = input_buffer_.data();
            if (HasEnoughEnergy(frame)) {
                float raw_angle = afe_doa_process(doa_, frame);
                if (std::isfinite(raw_angle) &&
                    raw_angle >= 0.0f && raw_angle <= 180.0f) {
                    reported_angle = ConvertAndSmooth(raw_angle);
                    has_report = true;

                    int64_t now = esp_timer_get_time();
                    if (now - last_log_time_us_ >= kLogIntervalUs) {
                        ESP_LOGI(TAG, "Sound direction: %.1f degrees",
                            reported_angle);
                        last_log_time_us_ = now;
                    }
                }
            }
            input_buffer_.erase(input_buffer_.begin(),
                input_buffer_.begin() + frame_size);
        }
        callback = direction_callback_;
    }

    if (has_report && callback) {
        callback(reported_angle);
    }
}

void SoundSourceLocator::Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    input_buffer_.clear();
    playback_active_ = false;
}

void SoundSourceLocator::OnDirection(
    std::function<void(float)> callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    direction_callback_ = std::move(callback);
}

bool SoundSourceLocator::HasEnoughEnergy(const int16_t* frame) const {
    int64_t sum_squares = 0;
    for (int i = 0; i < window_samples_; ++i) {
        int32_t left = frame[i * kInputChannels];
        int32_t right = frame[i * kInputChannels + 1];
        sum_squares += static_cast<int64_t>(left) * left;
        sum_squares += static_cast<int64_t>(right) * right;
    }
    float mean_square = static_cast<float>(sum_squares) /
        static_cast<float>(window_samples_ * 2);
    return mean_square >= minimum_rms_ * minimum_rms_;
}

float SoundSourceLocator::ConvertAndSmooth(float raw_angle) {
    float signed_angle = std::clamp(raw_angle - 90.0f, -90.0f, 90.0f);

    angle_history_[angle_history_position_] = signed_angle;
    angle_history_position_ =
        (angle_history_position_ + 1) % angle_history_.size();
    angle_history_count_ =
        std::min(angle_history_count_ + 1, angle_history_.size());

    auto sorted = angle_history_;
    std::sort(sorted.begin(), sorted.begin() + angle_history_count_);
    float median;
    if (angle_history_count_ % 2 == 0) {
        size_t upper = angle_history_count_ / 2;
        median = (sorted[upper - 1] + sorted[upper]) * 0.5f;
    } else {
        median = sorted[angle_history_count_ / 2];
    }

    if (has_smoothed_angle_) {
        smoothed_angle_ = smoothed_angle_ * 0.65f + median * 0.35f;
    } else {
        smoothed_angle_ = median;
        has_smoothed_angle_ = true;
    }
    return smoothed_angle_;
}
