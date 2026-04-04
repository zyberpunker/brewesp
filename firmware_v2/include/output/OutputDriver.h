#pragma once

#include <Arduino.h>

enum class OutputState {
    Off,
    On,
    Unknown,
};

class OutputDriver {
public:
    virtual ~OutputDriver() = default;

    virtual const char* driverName() const = 0;
    virtual bool begin() = 0;
    virtual bool setState(OutputState state) = 0;
    virtual OutputState getState() const = 0;
    virtual String describe() const = 0;
    virtual void update() {}
};
