#include "wifi_board.h"
#include "head_audio_codec.h"
#include "application.h"
#include "button.h"
#include "config.h"

#include <driver/i2c_master.h>
#include <esp_log.h>

#define TAG "HeadBoard"

class HeadBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t codec_i2c_bus_ = nullptr;
    Button boot_button_;

    void InitializeI2c() {
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
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&config, &codec_i2c_bus_));
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });

#if CONFIG_USE_DEVICE_AEC
        boot_button_.OnDoubleClick([]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateIdle) {
                app.SetAecMode(
                    app.GetAecMode() == kAecOff ? kAecOnDeviceSide : kAecOff);
            }
        });
#endif
    }

public:
    HeadBoard() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializeI2c();
        InitializeButtons();
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
