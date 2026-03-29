#include "controller/ControllerEngine.h"

namespace {
constexpr uint32_t kMsPerSecond = 1000UL;
}

void ControllerEngine::reset() {
    heatingLockedUntilMs_ = 0;
    coolingLockedUntilMs_ = 0;
    status_ = Status{};
    status_.reason = "idle";
}

bool ControllerEngine::update(const FermentationConfig& config, const Inputs& inputs, OutputManager& outputs) {
    status_.automaticControlActive = false;
    status_.heatingDemand = false;
    status_.coolingDemand = false;

    if (config.mode != "thermostat" && config.mode != "profile") {
        return setState(State::Idle, "mode inactive");
    }

    if (!inputs.hasPrimaryTemp) {
        return setState(State::WaitingForSensor, "awaiting primary sensor");
    }

    status_.automaticControlActive = true;

    const float setpointC = config.thermostat.setpointC;
    const float hysteresisC = config.thermostat.hysteresisC;
    const float heatThresholdC = setpointC - hysteresisC;
    const float coolThresholdC = setpointC + hysteresisC;

    if (inputs.primaryTempC <= heatThresholdC) {
        status_.heatingDemand = true;
    } else if (inputs.primaryTempC >= coolThresholdC) {
        status_.coolingDemand = true;
    }

    if (status_.heatingDemand) {
        const uint32_t remaining = remainingSeconds(heatingLockedUntilMs_, inputs.nowMs);
        if (remaining > 0) {
            return setState(State::LockedHeating, ("heating delay " + String(remaining) + "s").c_str());
        }

        const bool changed = outputs.setHeating(OutputState::On);
        outputs.setCooling(OutputState::Off);
        const bool stateChanged = setState(State::Heating, "heating");
        if (changed) {
            coolingLockedUntilMs_ =
                inputs.nowMs + static_cast<uint32_t>(config.thermostat.coolingDelaySeconds) * kMsPerSecond;
        }
        return stateChanged || changed;
    }

    if (status_.coolingDemand) {
        const uint32_t remaining = remainingSeconds(coolingLockedUntilMs_, inputs.nowMs);
        if (remaining > 0) {
            return setState(State::LockedCooling, ("cooling delay " + String(remaining) + "s").c_str());
        }

        const bool changed = outputs.setCooling(OutputState::On);
        outputs.setHeating(OutputState::Off);
        const bool stateChanged = setState(State::Cooling, "cooling");
        if (changed) {
            heatingLockedUntilMs_ =
                inputs.nowMs + static_cast<uint32_t>(config.thermostat.heatingDelaySeconds) * kMsPerSecond;
        }
        return stateChanged || changed;
    }

    const bool heatingWasOn = outputs.heatingState() == OutputState::On;
    const bool coolingWasOn = outputs.coolingState() == OutputState::On;
    const bool heatingChanged = heatingWasOn ? outputs.setHeating(OutputState::Off) : false;
    const bool coolingChanged = coolingWasOn ? outputs.setCooling(OutputState::Off) : false;

    if (heatingChanged) {
        coolingLockedUntilMs_ =
            inputs.nowMs + static_cast<uint32_t>(config.thermostat.coolingDelaySeconds) * kMsPerSecond;
    }
    if (coolingChanged) {
        heatingLockedUntilMs_ =
            inputs.nowMs + static_cast<uint32_t>(config.thermostat.heatingDelaySeconds) * kMsPerSecond;
    }

    const bool stateChanged = setState(State::Idle, "within hysteresis");
    return stateChanged || heatingChanged || coolingChanged;
}

const ControllerEngine::Status& ControllerEngine::status() const {
    return status_;
}

const char* ControllerEngine::stateName(State state) {
    switch (state) {
        case State::Idle:
            return "idle";
        case State::WaitingForSensor:
            return "waiting_for_sensor";
        case State::Heating:
            return "heating";
        case State::Cooling:
            return "cooling";
        case State::LockedHeating:
            return "locked_heating";
        case State::LockedCooling:
            return "locked_cooling";
        case State::Fault:
            return "fault";
        default:
            return "unknown";
    }
}

bool ControllerEngine::setState(State nextState, const String& reason) {
    const bool changed = status_.state != nextState || status_.reason != reason;
    status_.state = nextState;
    status_.reason = reason;
    return changed;
}

uint32_t ControllerEngine::remainingSeconds(uint32_t lockedUntilMs, uint32_t nowMs) const {
    if (lockedUntilMs == 0 || static_cast<int32_t>(lockedUntilMs - nowMs) <= 0) {
        return 0;
    }

    const uint32_t remainingMs = lockedUntilMs - nowMs;
    return (remainingMs + (kMsPerSecond - 1)) / kMsPerSecond;
}
