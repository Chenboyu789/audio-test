#include "wifi_board.h"
#include "head_audio_codec.h"
#include "pca9685.h"
#include "config.h"

#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <esp_check.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <cmath>
#include <memory>
#include <new>

#define TAG "HeadBoard"

class HeadBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t codec_i2c_bus_ = nullptr;
    i2c_master_bus_handle_t servo_i2c_bus_ = nullptr;
    std::unique_ptr<Pca9685> pca9685_;
    QueueHandle_t servo_target_queue_ = nullptr;
    TaskHandle_t servo_task_ = nullptr;
    float last_servo_angle_deg_ = SERVO_CENTER_DEG;
    bool servo_position_known_ = false;
    TickType_t last_servo_attempt_tick_ = 0;
    static constexpr float kServoStepDegrees =
        SERVO_TRACKING_MAX_SPEED_DEG_PER_SEC *
        SERVO_TRACKING_MIN_INTERVAL_MS / 1000.0f;

    esp_err_t InitializeServoPower() {
        ESP_RETURN_ON_ERROR(gpio_set_level(PCA9685_POWER_ENABLE_PIN, 1), TAG,
                            "failed to preset servo power enable");

        gpio_config_t config = {
            .pin_bit_mask = 1ULL << PCA9685_POWER_ENABLE_PIN,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&config), TAG,
                            "failed to configure servo power enable");
        return gpio_set_level(PCA9685_POWER_ENABLE_PIN, 1);
    }

    void InitializeCodecI2c() {
        i2c_master_bus_config_t config = {
            .i2c_port = I2C_NUM_1,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
                .allow_pd = 0,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&config, &codec_i2c_bus_));
    }

    static void ServoTaskEntry(void* arg) {
        static_cast<HeadBoard*>(arg)->ServoTaskLoop();
    }

    void ServoTaskLoop() {
        float target_angle_deg = SERVO_CENTER_DEG;
        const TickType_t minimum_interval =
            pdMS_TO_TICKS(SERVO_TRACKING_MIN_INTERVAL_MS) + 1;

        while (true) {
            if (xQueueReceive(servo_target_queue_, &target_angle_deg, portMAX_DELAY) !=
                pdTRUE) {
                continue;
            }

            while (true) {
                float latest_angle_deg = target_angle_deg;
                while (xQueueReceive(servo_target_queue_, &latest_angle_deg, 0) ==
                       pdTRUE) {
                    target_angle_deg = latest_angle_deg;
                }

                if (!servo_position_known_) {
                    const TickType_t now = xTaskGetTickCount();
                    const TickType_t elapsed = now - last_servo_attempt_tick_;
                    if (elapsed < minimum_interval) {
                        if (xQueueReceive(servo_target_queue_, &latest_angle_deg,
                                          minimum_interval - elapsed) == pdTRUE) {
                            target_angle_deg = latest_angle_deg;
                            continue;
                        }
                    }
                    while (xQueueReceive(servo_target_queue_, &latest_angle_deg, 0) ==
                           pdTRUE) {
                        target_angle_deg = latest_angle_deg;
                    }

                    last_servo_attempt_tick_ = xTaskGetTickCount();
                    esp_err_t err =
                        pca9685_->SetAngle(SERVO_CHANNEL, target_angle_deg);
                    if (err != ESP_OK) {
                        ESP_LOGE(TAG, "Failed to set initial servo angle %.1f: %s",
                                 target_angle_deg, esp_err_to_name(err));
                        break;
                    }
                    last_servo_angle_deg_ = target_angle_deg;
                    servo_position_known_ = true;
                    break;
                }

                float angle_delta = target_angle_deg - last_servo_angle_deg_;
                if (std::fabs(angle_delta) < SERVO_TRACKING_DEADBAND_DEG) {
                    break;
                }

                const TickType_t now = xTaskGetTickCount();
                const TickType_t elapsed = now - last_servo_attempt_tick_;
                if (elapsed < minimum_interval) {
                    if (xQueueReceive(servo_target_queue_, &latest_angle_deg,
                                      minimum_interval - elapsed) == pdTRUE) {
                        target_angle_deg = latest_angle_deg;
                        continue;
                    }
                }

                while (xQueueReceive(servo_target_queue_, &latest_angle_deg, 0) ==
                       pdTRUE) {
                    target_angle_deg = latest_angle_deg;
                }
                angle_delta = target_angle_deg - last_servo_angle_deg_;
                if (std::fabs(angle_delta) < SERVO_TRACKING_DEADBAND_DEG) {
                    break;
                }

                const float step_degrees = std::copysign(
                    std::fmin(std::fabs(angle_delta), kServoStepDegrees),
                    angle_delta);
                const float next_angle_deg =
                    last_servo_angle_deg_ + step_degrees;

                last_servo_attempt_tick_ = xTaskGetTickCount();
                esp_err_t err =
                    pca9685_->SetAngle(SERVO_CHANNEL, next_angle_deg);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to track servo angle %.1f: %s",
                             next_angle_deg, esp_err_to_name(err));
                    break;
                }

                last_servo_angle_deg_ = next_angle_deg;
            }
        }
    }

    void StartServoTracking() {
        servo_target_queue_ = xQueueCreate(1, sizeof(float));
        if (servo_target_queue_ == nullptr) {
            ESP_LOGE(TAG, "Failed to create servo target queue");
            return;
        }

        BaseType_t task_created = xTaskCreate(
            ServoTaskEntry, "head_servo", 3072, this, 2, &servo_task_);
        if (task_created != pdPASS) {
            ESP_LOGE(TAG, "Failed to create servo tracking task");
            vQueueDelete(servo_target_queue_);
            servo_target_queue_ = nullptr;
            servo_task_ = nullptr;
            return;
        }
        ESP_LOGI(TAG, "Servo sound tracking started");
    }

    void InitializeServo() {
        i2c_master_bus_config_t config = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = SERVO_I2C_SDA_PIN,
            .scl_io_num = SERVO_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
                .allow_pd = 0,
            },
        };
        esp_err_t err = i2c_new_master_bus(&config, &servo_i2c_bus_);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize servo I2C bus: %s",
                     esp_err_to_name(err));
            return;
        }

        err = gpio_set_level(PCA9685_POWER_ENABLE_PIN, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to enable PCA9685 power: %s", esp_err_to_name(err));
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(10));

        pca9685_.reset(
            new (std::nothrow) Pca9685(servo_i2c_bus_, PCA9685_I2C_ADDRESS));
        if (!pca9685_) {
            ESP_LOGE(TAG, "Failed to allocate PCA9685 driver");
            return;
        }
        err = pca9685_->Initialize(PCA9685_PWM_FREQUENCY_HZ);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize PCA9685: %s", esp_err_to_name(err));
            return;
        }

        last_servo_attempt_tick_ = xTaskGetTickCount();
        err = pca9685_->SetAngle(SERVO_CHANNEL, SERVO_CENTER_DEG);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to restore initial servo angle: %s",
                     esp_err_to_name(err));
        } else {
            last_servo_angle_deg_ = SERVO_CENTER_DEG;
            servo_position_known_ = true;
            vTaskDelay(pdMS_TO_TICKS(SERVO_STARTUP_CENTER_SETTLE_MS));
        }
        StartServoTracking();
    }

public:
    HeadBoard() {
        esp_err_t servo_power_err = InitializeServoPower();
        if (servo_power_err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize servo power: %s",
                     esp_err_to_name(servo_power_err));
        }
        InitializeCodecI2c();
        if (servo_power_err == ESP_OK) {
            InitializeServo();
        }
        ESP_LOGI(TAG, "Head board initialized");
    }

    void OnSoundDirection(float angle_deg, float confidence) override {
        (void)confidence;
        if (servo_target_queue_ == nullptr || !std::isfinite(angle_deg)) {
            return;
        }

        const float clamped_sound_angle =
            std::fmax(-90.0f, std::fmin(90.0f, angle_deg));
        const float mapped_angle = clamped_sound_angle < 0.0f
            ? SERVO_CENTER_DEG +
                clamped_sound_angle *
                    (SERVO_CENTER_DEG - SERVO_LEFT_LIMIT_DEG) / 90.0f
            : SERVO_CENTER_DEG +
                clamped_sound_angle *
                    (SERVO_RIGHT_LIMIT_DEG - SERVO_CENTER_DEG) / 90.0f;
        const float target_angle = std::fmax(
            SERVO_LEFT_LIMIT_DEG, std::fmin(SERVO_RIGHT_LIMIT_DEG, mapped_angle));
        xQueueOverwrite(servo_target_queue_, &target_angle);
    }

    AudioCodec* GetAudioCodec() override {
        static HeadAudioCodec audio_codec(
            codec_i2c_bus_,
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK,
            AUDIO_I2S_GPIO_BCLK,
            AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN,
            AUDIO_CODEC_ES8311_ADDR,
            AUDIO_CODEC_ES7210_ADDR);
        return &audio_codec;
    }
};

DECLARE_BOARD(HeadBoard);
