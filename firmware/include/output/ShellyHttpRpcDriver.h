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
    void update() override;

private:
    bool queryState(const char* context = nullptr);
    bool performGet(const String& path, String& response);
    bool parseOutputState(const String& response, OutputState& state) const;
    bool responseHasRpcError(const String& response, String& message) const;
    String buildUrl(const String& path) const;

    String host_;
    uint16_t port_;
    uint8_t switchId_;
    OutputState state_ = OutputState::Unknown;
    uint32_t lastPollMs_ = 0;
};
