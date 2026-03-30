#pragma once

#include <Arduino.h>

#include "config/FermentationConfig.h"
#include "output/OutputManager.h"

class ControllerEngine {
public:
    enum class State {
        Idle,
        WaitingForSensor,
        Heating,
        Cooling,
        LockedHeating,
        LockedCooling,
        Fault,
    };

    struct Inputs {
        bool hasPrimaryTemp = false;
        float primaryTempC = 0.0f;
        bool hasSecondaryTemp = false;
        float secondaryTempC = 0.0f;
        uint32_t nowMs = 0;
    };

    struct Status {
        State state = State::Idle;
        bool automaticControlActive = false;
        bool heatingDemand = false;
        bool coolingDemand = false;
        String reason = "idle";
    };

    void reset();
    bool update(const FermentationConfig& config, const Inputs& inputs, OutputManager& outputs);
    const Status& status() const;

    static const char* stateName(State state);

private:
    bool forceOutputsOff(OutputManager& outputs);
    bool setState(State nextState, const String& reason);
    uint32_t remainingSeconds(uint32_t lockedUntilMs, uint32_t nowMs) const;
    void updateSecondaryLimits(const FermentationConfig& config, const Inputs& inputs);

    Status status_;
    uint32_t heatingLockedUntilMs_ = 0;
    uint32_t coolingLockedUntilMs_ = 0;
    bool heatingLimitedBySecondary_ = false;
    bool coolingLimitedBySecondary_ = false;
};
