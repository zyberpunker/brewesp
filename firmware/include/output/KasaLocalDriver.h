#pragma once

#include "output/OutputDriver.h"

class KasaLocalDriver : public OutputDriver {
public:
    KasaLocalDriver(String host, uint16_t port, String alias, uint32_t pollIntervalSeconds);

    const char* driverName() const override;
    bool begin() override;
    bool setState(OutputState state) override;
    OutputState getState() const override;
    String describe() const override;
    void update() override;

private:
    bool queryState(const char* context = nullptr);
    bool sendRequest(const String& payload, String& response);
    OutputState parseRelayState(const String& response) const;
    int parseErrCode(const String& response) const;

    String host_;
    uint16_t port_;
    String alias_;
    uint32_t pollIntervalSeconds_;
    OutputState state_ = OutputState::Unknown;
    OutputState lastLoggedState_ = OutputState::Unknown;
    uint32_t lastPollMs_ = 0;
};
