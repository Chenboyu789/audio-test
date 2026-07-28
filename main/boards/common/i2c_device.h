#ifndef I2C_DEVICE_H
#define I2C_DEVICE_H

#include <driver/i2c_master.h>
#include <esp_err.h>

class I2cDevice {
public:
    I2cDevice(i2c_master_bus_handle_t i2c_bus, uint8_t addr);

protected:
    I2cDevice() = default;

    i2c_master_dev_handle_t i2c_device_ = nullptr;

    esp_err_t InitializeI2cDevice(i2c_master_bus_handle_t i2c_bus, uint8_t addr);
    void WriteReg(uint8_t reg, uint8_t value);
    uint8_t ReadReg(uint8_t reg);
    void ReadRegs(uint8_t reg, uint8_t* buffer, size_t length);
};

#endif // I2C_DEVICE_H
