#pragma once

#include <memory>

#include "config/SystemConfig.h"
#include "output/OutputDriver.h"

class OutputManager {
public:
    bool begin(const SystemConfig& config);
    bool applyConfig(const SystemConfig& config);
    void update();
    void refreshStates();

    bool setHeating(OutputState state);
    bool setCooling(OutputState state);

    OutputState heatingState() const;
    OutputState coolingState() const;

    String describeHeating() const;
    String describeCooling() const;

private:
    std::unique_ptr<OutputDriver> createDriver(const OutputConfig& config);
    bool applyHeatingIfDue(uint32_t nowMs);
    bool applyCoolingIfDue(uint32_t nowMs);

    std::unique_ptr<OutputDriver> heating_;
    std::unique_ptr<OutputDriver> cooling_;
    OutputState desiredHeatingState_ = OutputState::Unknown;
    OutputState desiredCoolingState_ = OutputState::Unknown;
    uint32_t nextHeatingAttemptAtMs_ = 0;
    uint32_t nextCoolingAttemptAtMs_ = 0;
};
