#ifndef _HEAD_PCA9685_H_
#define _HEAD_PCA9685_H_

#include "i2c_device.h"

#include <esp_err.h>

#include <cstddef>
#include <cstdint>

class Pca9685 : public I2cDevice {
public:
    Pca9685(i2c_master_bus_handle_t i2c_bus, uint8_t address);

    esp_err_t Initialize(uint16_t frequency_hz = 50);
    esp_err_t SetPwm(uint8_t channel, uint16_t on_count, uint16_t off_count);
    esp_err_t SetPulseWidthUs(uint8_t channel, uint16_t pulse_width_us);
    esp_err_t SetAngle(uint8_t channel, float angle_degrees);
    esp_err_t DisableChannel(uint8_t channel);
    esp_err_t DisableAll();

private:
    static constexpr uint8_t kChannelCount = 16;
    static constexpr uint16_t kPwmResolution = 4096;
    static constexpr uint16_t kMinimumPulseWidthUs = 500;
    static constexpr uint16_t kMaximumPulseWidthUs = 2500;

    i2c_master_bus_handle_t i2c_bus_;
    uint8_t address_;
    uint16_t frequency_hz_ = 0;

    esp_err_t WriteRegister(uint8_t reg, uint8_t value);
    esp_err_t ReadRegister(uint8_t reg, uint8_t* value);
    esp_err_t WriteRegisters(uint8_t start_reg, const uint8_t* data, size_t length);
};

#endif  // _HEAD_PCA9685_H_
