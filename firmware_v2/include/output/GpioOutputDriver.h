#pragma once

#include "output/OutputDriver.h"

class GpioOutputDriver : public OutputDriver {
public:
    GpioOutputDriver(uint8_t pin, bool activeHigh);

    const char* driverName() const override;
    bool begin() override;
    bool setState(OutputState state) override;
    OutputState getState() const override;
    String describe() const override;

private:
    uint8_t pin_;
    bool activeHigh_;
    OutputState state_ = OutputState::Unknown;
};
