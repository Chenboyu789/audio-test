#include "pca9685.h"

#include <driver/i2c_master.h>
#include <esp_check.h>
#include <esp_rom_sys.h>

#include <cmath>
#include <cstdint>

#define TAG "Pca9685"

namespace {

constexpr uint8_t kMode1Register = 0x00;
constexpr uint8_t kMode2Register = 0x01;
constexpr uint8_t kLed0OnLowRegister = 0x06;
constexpr uint8_t kPrescaleRegister = 0xFE;

constexpr uint8_t kMode1Restart = 0x80;
constexpr uint8_t kMode1AutoIncrement = 0x20;
constexpr uint8_t kMode1Sleep = 0x10;
constexpr uint8_t kMode2TotemPole = 0x04;
constexpr uint8_t kFullOff = 0x10;

constexpr uint32_t kOscillatorFrequencyHz = 25000000;
constexpr uint8_t kMinimumPrescale = 3;

}  // namespace

Pca9685::Pca9685(i2c_master_bus_handle_t i2c_bus, uint8_t address)
    : i2c_bus_(i2c_bus), address_(address) {
}

esp_err_t Pca9685::WriteRegister(uint8_t reg, uint8_t value) {
    const uint8_t data[] = {reg, value};
    return i2c_master_transmit(i2c_device_, data, sizeof(data), 100);
}

esp_err_t Pca9685::ReadRegister(uint8_t reg, uint8_t* value) {
    ESP_RETURN_ON_FALSE(value != nullptr, ESP_ERR_INVALID_ARG, TAG, "value is null");
    return i2c_master_transmit_receive(i2c_device_, &reg, 1, value, 1, 100);
}

esp_err_t Pca9685::WriteRegisters(uint8_t start_reg, const uint8_t* data, size_t length) {
    ESP_RETURN_ON_FALSE(data != nullptr, ESP_ERR_INVALID_ARG, TAG, "data is null");
    ESP_RETURN_ON_FALSE(length <= 4, ESP_ERR_INVALID_SIZE, TAG, "write is too large");

    uint8_t buffer[5] = {start_reg};
    for (size_t i = 0; i < length; ++i) {
        buffer[i + 1] = data[i];
    }
    return i2c_master_transmit(i2c_device_, buffer, length + 1, 100);
}

esp_err_t Pca9685::Initialize(uint16_t frequency_hz) {
    ESP_RETURN_ON_FALSE(frequency_hz > 0, ESP_ERR_INVALID_ARG, TAG,
                        "frequency must be greater than zero");

    if (i2c_device_ == nullptr) {
        ESP_RETURN_ON_ERROR(InitializeI2cDevice(i2c_bus_, address_), TAG,
                            "failed to add I2C device");
    }

    const uint32_t denominator = static_cast<uint32_t>(frequency_hz) * kPwmResolution;
    const uint32_t prescale_value =
        (kOscillatorFrequencyHz + denominator / 2) / denominator - 1;
    ESP_RETURN_ON_FALSE(
        prescale_value >= kMinimumPrescale && prescale_value <= UINT8_MAX,
        ESP_ERR_INVALID_ARG, TAG, "frequency %u Hz is out of range", frequency_hz);

    uint8_t old_mode = 0;
    ESP_RETURN_ON_ERROR(ReadRegister(kMode1Register, &old_mode), TAG,
                        "failed to read MODE1");

    const uint8_t sleep_mode = (old_mode & ~kMode1Restart) | kMode1Sleep;
    ESP_RETURN_ON_ERROR(WriteRegister(kMode1Register, sleep_mode), TAG,
                        "failed to enter sleep mode");
    ESP_RETURN_ON_ERROR(
        WriteRegister(kPrescaleRegister, static_cast<uint8_t>(prescale_value)), TAG,
        "failed to set prescale");
    ESP_RETURN_ON_ERROR(WriteRegister(kMode2Register, kMode2TotemPole), TAG,
                        "failed to set MODE2");

    const uint8_t wake_mode = (old_mode & ~kMode1Sleep) | kMode1AutoIncrement;
    ESP_RETURN_ON_ERROR(WriteRegister(kMode1Register, wake_mode), TAG,
                        "failed to leave sleep mode");
    esp_rom_delay_us(500);
    ESP_RETURN_ON_ERROR(WriteRegister(kMode1Register, wake_mode | kMode1Restart), TAG,
                        "failed to restart PWM");

    ESP_RETURN_ON_ERROR(DisableAll(), TAG, "failed to disable channel outputs");
    frequency_hz_ = frequency_hz;
    return ESP_OK;
}

esp_err_t Pca9685::SetPwm(uint8_t channel, uint16_t on_count, uint16_t off_count) {
    ESP_RETURN_ON_FALSE(channel < kChannelCount, ESP_ERR_INVALID_ARG, TAG,
                        "invalid channel %u", channel);
    ESP_RETURN_ON_FALSE(on_count < kPwmResolution && off_count < kPwmResolution,
                        ESP_ERR_INVALID_ARG, TAG, "PWM count is out of range");

    const uint8_t data[] = {
        static_cast<uint8_t>(on_count & 0xFF),
        static_cast<uint8_t>((on_count >> 8) & 0x0F),
        static_cast<uint8_t>(off_count & 0xFF),
        static_cast<uint8_t>((off_count >> 8) & 0x0F),
    };
    return WriteRegisters(kLed0OnLowRegister + channel * 4, data, sizeof(data));
}

esp_err_t Pca9685::SetPulseWidthUs(uint8_t channel, uint16_t pulse_width_us) {
    ESP_RETURN_ON_FALSE(frequency_hz_ > 0, ESP_ERR_INVALID_STATE, TAG,
                        "device is not initialized");
    ESP_RETURN_ON_FALSE(channel < kChannelCount, ESP_ERR_INVALID_ARG, TAG,
                        "invalid channel %u", channel);

    const uint32_t period_us = 1000000U / frequency_hz_;
    ESP_RETURN_ON_FALSE(pulse_width_us < period_us, ESP_ERR_INVALID_ARG, TAG,
                        "pulse width %u us exceeds PWM period", pulse_width_us);

    const uint64_t scaled =
        static_cast<uint64_t>(pulse_width_us) * frequency_hz_ * kPwmResolution;
    const uint16_t off_count = static_cast<uint16_t>((scaled + 500000U) / 1000000U);
    ESP_RETURN_ON_FALSE(off_count < kPwmResolution, ESP_ERR_INVALID_ARG, TAG,
                        "pulse width produces an invalid PWM count");
    return SetPwm(channel, 0, off_count);
}

esp_err_t Pca9685::SetAngle(uint8_t channel, float angle_degrees) {
    ESP_RETURN_ON_FALSE(std::isfinite(angle_degrees) && angle_degrees >= 0.0f &&
                            angle_degrees <= 180.0f,
                        ESP_ERR_INVALID_ARG, TAG, "angle is out of range");

    const float pulse_width =
        kMinimumPulseWidthUs +
        angle_degrees * (kMaximumPulseWidthUs - kMinimumPulseWidthUs) / 180.0f;
    return SetPulseWidthUs(channel, static_cast<uint16_t>(pulse_width + 0.5f));
}

esp_err_t Pca9685::DisableChannel(uint8_t channel) {
    ESP_RETURN_ON_FALSE(channel < kChannelCount, ESP_ERR_INVALID_ARG, TAG,
                        "invalid channel %u", channel);
    const uint8_t data[] = {0, 0, 0, kFullOff};
    return WriteRegisters(kLed0OnLowRegister + channel * 4, data, sizeof(data));
}

esp_err_t Pca9685::DisableAll() {
    for (uint8_t channel = 0; channel < kChannelCount; ++channel) {
        ESP_RETURN_ON_ERROR(DisableChannel(channel), TAG,
                            "failed to disable channel %u", channel);
    }
    return ESP_OK;
}
