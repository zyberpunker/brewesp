#include "output/GpioOutputDriver.h"

GpioOutputDriver::GpioOutputDriver(uint8_t pin, bool activeHigh)
    : pin_(pin), activeHigh_(activeHigh) {}

const char* GpioOutputDriver::driverName() const {
    return "gpio";
}

bool GpioOutputDriver::begin() {
    pinMode(pin_, OUTPUT);
    return setState(OutputState::Off);
}

bool GpioOutputDriver::setState(OutputState state) {
    if (state == OutputState::Unknown) {
        return false;
    }

    const bool logicalOn = (state == OutputState::On);
    const uint8_t electricalLevel = logicalOn == activeHigh_ ? HIGH : LOW;
    digitalWrite(pin_, electricalLevel);
    state_ = state;
    return true;
}

OutputState GpioOutputDriver::getState() const {
    return state_;
}

String GpioOutputDriver::describe() const {
    return "gpio(pin=" + String(pin_) + ", state=" + String(state_ == OutputState::On ? "on" : "off") + ")";
}
