#include "wifi_board.h"
#include "head_audio_codec.h"
#include "pca9685.h"
#include "config.h"

#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <esp_check.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <memory>
#include <new>

#define TAG "HeadBoard"

class HeadBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t codec_i2c_bus_ = nullptr;
    i2c_master_bus_handle_t servo_i2c_bus_ = nullptr;
    std::unique_ptr<Pca9685> pca9685_;

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

    esp_err_t RunServoTest() {
        static constexpr float kTestAngles[] = {0.0f, 90.0f, 180.0f, 90.0f};
        for (size_t i = 0; i < sizeof(kTestAngles) / sizeof(kTestAngles[0]); ++i) {
            esp_err_t err = pca9685_->SetAngle(PCA9685_TEST_CHANNEL, kTestAngles[i]);
            if (err != ESP_OK) {
                return err;
            }
            ESP_LOGI(TAG, "PCA9685 CH%d test angle: %.0f degrees",
                     PCA9685_TEST_CHANNEL, kTestAngles[i]);
            if (i + 1 < sizeof(kTestAngles) / sizeof(kTestAngles[0])) {
                vTaskDelay(pdMS_TO_TICKS(800));
            }
        }
        return ESP_OK;
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

        err = RunServoTest();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "PCA9685 CH%d test failed: %s",
                     PCA9685_TEST_CHANNEL, esp_err_to_name(err));
            return;
        }
        ESP_LOGI(TAG, "PCA9685 CH%d test completed", PCA9685_TEST_CHANNEL);
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
