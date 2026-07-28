#ifndef SOUND_SOURCE_LOCATOR_H
#define SOUND_SOURCE_LOCATOR_H

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

struct SoundDirectionResult {
    float angle_deg;
    float confidence;
};

class SoundSourceLocator {
public:
    using DirectionCallback = std::function<void(const SoundDirectionResult&)>;

    SoundSourceLocator(int input_channels, DirectionCallback callback);
    ~SoundSourceLocator();

    bool IsReady() const;
    void Feed(const std::vector<int16_t>& interleaved, bool playback_active);

private:
    static constexpr size_t kFrameSamples = 1024;
    static constexpr size_t kChunkSamples = 160;
    static constexpr size_t kAngleHistorySize = 5;

    struct StereoChunk {
        uint16_t samples = 0;
        std::array<int16_t, kChunkSamples> left{};
        std::array<int16_t, kChunkSamples> right{};
    };

    static void TaskEntry(void* arg);
    void TaskLoop();
    void ProcessFrame();
    float SmoothAngle(float angle);

    int input_channels_;
    DirectionCallback callback_;
    void* doa_ = nullptr;
    QueueHandle_t queue_ = nullptr;
    SemaphoreHandle_t task_exited_ = nullptr;
    TaskHandle_t task_ = nullptr;
    std::atomic<bool> running_{false};
    std::atomic<bool> playback_active_{false};
    std::atomic<bool> reset_requested_{false};

    std::array<int16_t, kFrameSamples> left_frame_{};
    std::array<int16_t, kFrameSamples> right_frame_{};
    size_t frame_samples_ = 0;
    float noise_floor_rms_ = 100.0f;
    int noise_calibration_frames_ = 0;
    int consecutive_speech_frames_ = 0;
    std::array<float, kAngleHistorySize> angle_history_{};
    size_t angle_history_count_ = 0;
    size_t angle_history_index_ = 0;
    float filtered_angle_ = 0.0f;
    bool has_filtered_angle_ = false;
};

#endif
