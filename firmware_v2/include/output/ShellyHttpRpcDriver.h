#pragma once

#include "output/OutputDriver.h"

class ShellyHttpRpcDriver : public OutputDriver {
public:
    ShellyHttpRpcDriver(String host, uint16_t port, uint8_t switchId);

    const char* driverName() const override;
    bool begin() override;
    bool setState(OutputState state) override;
    OutputState getState() const override;
    String describe() const override;

private:
    String host_;
    uint16_t port_;
    uint8_t switchId_;
    OutputState state_ = OutputState::Unknown;
};
