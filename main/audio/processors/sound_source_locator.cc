#include "sound_source_locator.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include <esp_doa.h>
#include <esp_log.h>

#define TAG "SoundSourceLocator"

namespace {
constexpr int kSampleRate = 16000;
constexpr float kAngularResolutionDeg = 5.0f;
constexpr float kMicrophoneSpacingM = 0.045f;
constexpr float kMinimumSpeechRms = 300.0f;
constexpr float kMaximumNoiseFloorRms = 600.0f;
constexpr float kSpeechToNoiseRatio = 2.0f;
constexpr float kMinimumSpeechZeroCrossingRate = 0.01f;
constexpr float kMaximumSpeechZeroCrossingRate = 0.35f;
constexpr float kFilterAlpha = 0.35f;
constexpr UBaseType_t kQueueLength = 4;
}  // namespace

SoundSourceLocator::SoundSourceLocator(
    int input_channels, DirectionCallback callback)
    : input_channels_(input_channels), callback_(std::move(callback)) {
    if (input_channels_ < 3) {
        ESP_LOGE(TAG, "Head MRM input requires three channels");
        return;
    }

    doa_ = esp_doa_create(
        kSampleRate, kAngularResolutionDeg, kMicrophoneSpacingM, kFrameSamples);
    if (doa_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create SRP-PHAT processor");
        return;
    }

    queue_ = xQueueCreate(kQueueLength, sizeof(StereoChunk));
    if (queue_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create input queue");
        esp_doa_destroy(static_cast<doa_handle_t*>(doa_));
        doa_ = nullptr;
        return;
    }
    task_exited_ = xSemaphoreCreateBinary();
    if (task_exited_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create task completion semaphore");
        vQueueDelete(queue_);
        queue_ = nullptr;
        esp_doa_destroy(static_cast<doa_handle_t*>(doa_));
        doa_ = nullptr;
        return;
    }

    running_.store(true);
    BaseType_t task_result = xTaskCreatePinnedToCore(
        TaskEntry, "sound_locator", 6144, this, 2, &task_, 1);
    if (task_result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create processing task");
        running_.store(false);
        vSemaphoreDelete(task_exited_);
        task_exited_ = nullptr;
        vQueueDelete(queue_);
        queue_ = nullptr;
        esp_doa_destroy(static_cast<doa_handle_t*>(doa_));
        doa_ = nullptr;
        task_ = nullptr;
        return;
    }

    ESP_LOGI(TAG, "SRP-PHAT initialized: 16 kHz, 45 mm, 5 degree resolution");
}

SoundSourceLocator::~SoundSourceLocator() {
    running_.store(false);
    if (queue_ != nullptr) {
        StereoChunk wakeup;
        xQueueSend(queue_, &wakeup, 0);
    }
    if (task_ != nullptr && task_exited_ != nullptr &&
        xSemaphoreTake(task_exited_, pdMS_TO_TICKS(500)) != pdTRUE) {
        vTaskDelete(task_);
    }
    task_ = nullptr;
    if (task_exited_ != nullptr) {
        vSemaphoreDelete(task_exited_);
        task_exited_ = nullptr;
    }
    if (queue_ != nullptr) {
        vQueueDelete(queue_);
        queue_ = nullptr;
    }
    if (doa_ != nullptr) {
        esp_doa_destroy(static_cast<doa_handle_t*>(doa_));
        doa_ = nullptr;
    }
}

bool SoundSourceLocator::IsReady() const {
    return doa_ != nullptr && queue_ != nullptr && running_.load();
}

void SoundSourceLocator::Feed(
    const std::vector<int16_t>& interleaved, bool playback_active) {
    if (!IsReady()) {
        return;
    }

    playback_active_.store(playback_active);
    if (playback_active) {
        reset_requested_.store(true);
        xQueueReset(queue_);
        return;
    }

    const size_t total_samples = interleaved.size() / input_channels_;
    size_t sample_offset = 0;
    while (sample_offset < total_samples) {
        StereoChunk chunk;
        chunk.samples = static_cast<uint16_t>(
            std::min(kChunkSamples, total_samples - sample_offset));
        for (size_t i = 0; i < chunk.samples; ++i) {
            const size_t base = (sample_offset + i) * input_channels_;
            chunk.left[i] = interleaved[base];
            const int16_t right = interleaved[base + 2];
            // Head TDM order is MIC1, reference, MIC2. MIC2 is wired with
            // opposite polarity, so normalize it before SRP-PHAT.
            chunk.right[i] = right == INT16_MIN
                ? INT16_MAX
                : static_cast<int16_t>(-right);
        }

        if (xQueueSend(queue_, &chunk, 0) != pdTRUE) {
            reset_requested_.store(true);
            StereoChunk discarded;
            xQueueReceive(queue_, &discarded, 0);
            xQueueSend(queue_, &chunk, 0);
        }
        sample_offset += chunk.samples;
    }
}

void SoundSourceLocator::TaskEntry(void* arg) {
    static_cast<SoundSourceLocator*>(arg)->TaskLoop();
}

void SoundSourceLocator::TaskLoop() {
    StereoChunk chunk;
    while (running_.load()) {
        if (xQueueReceive(queue_, &chunk, pdMS_TO_TICKS(50)) != pdTRUE) {
            continue;
        }
        if (!running_.load()) {
            break;
        }
        if (reset_requested_.exchange(false) || playback_active_.load()) {
            frame_samples_ = 0;
            consecutive_speech_frames_ = 0;
            angle_history_count_ = 0;
            angle_history_index_ = 0;
            has_filtered_angle_ = false;
            continue;
        }

        size_t chunk_offset = 0;
        while (chunk_offset < chunk.samples) {
            const size_t copy_samples = std::min(
                kFrameSamples - frame_samples_,
                static_cast<size_t>(chunk.samples) - chunk_offset);
            std::copy_n(
                chunk.left.begin() + chunk_offset, copy_samples,
                left_frame_.begin() + frame_samples_);
            std::copy_n(
                chunk.right.begin() + chunk_offset, copy_samples,
                right_frame_.begin() + frame_samples_);
            frame_samples_ += copy_samples;
            chunk_offset += copy_samples;

            if (frame_samples_ == kFrameSamples) {
                ProcessFrame();
                frame_samples_ = 0;
            }
        }
    }

    if (task_exited_ != nullptr) {
        xSemaphoreGive(task_exited_);
    }
    vTaskDelete(nullptr);
}

void SoundSourceLocator::ProcessFrame() {
    if (playback_active_.load()) {
        return;
    }

    double energy = 0.0;
    size_t zero_crossings = 0;
    int32_t previous_mono = 0;
    for (size_t i = 0; i < kFrameSamples; ++i) {
        const double left = left_frame_[i];
        const double right = right_frame_[i];
        energy += left * left + right * right;
        const int32_t mono =
            static_cast<int32_t>(left_frame_[i]) + right_frame_[i];
        if (i > 0 && ((mono >= 0) != (previous_mono >= 0))) {
            ++zero_crossings;
        }
        previous_mono = mono;
    }
    const float rms = static_cast<float>(
        std::sqrt(energy / (2.0 * kFrameSamples)));

    if (noise_calibration_frames_ < 8) {
        const float calibration_rms = std::clamp(
            rms, 1.0f, kMaximumNoiseFloorRms);
        noise_floor_rms_ =
            noise_floor_rms_ * 0.8f + calibration_rms * 0.2f;
        ++noise_calibration_frames_;
        return;
    }

    const float speech_threshold = std::max(
        kMinimumSpeechRms, noise_floor_rms_ * kSpeechToNoiseRatio);
    if (rms < speech_threshold) {
        noise_floor_rms_ =
            noise_floor_rms_ * 0.98f +
            std::clamp(rms, 1.0f, kMaximumNoiseFloorRms) * 0.02f;
        consecutive_speech_frames_ = 0;
        return;
    }
    const float zero_crossing_rate = static_cast<float>(zero_crossings) /
        static_cast<float>(kFrameSamples - 1);
    if (zero_crossing_rate < kMinimumSpeechZeroCrossingRate ||
        zero_crossing_rate > kMaximumSpeechZeroCrossingRate) {
        consecutive_speech_frames_ = 0;
        return;
    }
    if (++consecutive_speech_frames_ < 2) {
        return;
    }

    const float raw_angle = esp_doa_process(
        static_cast<doa_handle_t*>(doa_),
        left_frame_.data(), right_frame_.data());
    if (!std::isfinite(raw_angle) || raw_angle < 0.0f || raw_angle > 180.0f) {
        ESP_LOGW(TAG, "Invalid DOA result: %.2f", raw_angle);
        return;
    }

    // ESP-SR uses 0°=left, 90°=front and 180°=right.
    const float signed_angle = std::clamp(raw_angle - 90.0f, -90.0f, 90.0f);
    const float previous_angle = has_filtered_angle_ ? filtered_angle_ : signed_angle;
    const float smoothed_angle = SmoothAngle(signed_angle);
    const float snr = rms / std::max(noise_floor_rms_, 1.0f);
    const float energy_confidence = std::clamp(
        (snr - kSpeechToNoiseRatio) / 6.0f, 0.0f, 1.0f);
    const float stability_confidence = std::clamp(
        1.0f - std::abs(smoothed_angle - previous_angle) / 90.0f,
        0.0f, 1.0f);
    const float confidence =
        energy_confidence * 0.65f + stability_confidence * 0.35f;
    if (energy_confidence < 0.10f || confidence < 0.35f ||
        playback_active_.load()) {
        return;
    }

    if (callback_) {
        callback_({smoothed_angle, confidence});
    }
}

float SoundSourceLocator::SmoothAngle(float angle) {
    angle_history_[angle_history_index_] = angle;
    angle_history_index_ = (angle_history_index_ + 1) % kAngleHistorySize;
    angle_history_count_ = std::min(
        angle_history_count_ + 1, kAngleHistorySize);

    auto sorted = angle_history_;
    std::sort(sorted.begin(), sorted.begin() + angle_history_count_);
    const float median = sorted[angle_history_count_ / 2];
    if (!has_filtered_angle_) {
        filtered_angle_ = median;
        has_filtered_angle_ = true;
    } else {
        filtered_angle_ =
            filtered_angle_ * (1.0f - kFilterAlpha) + median * kFilterAlpha;
    }
    return filtered_angle_;
}
