#include "controller/ControllerEngine.h"

namespace {
constexpr uint32_t kMsPerSecond = 1000UL;
}

void ControllerEngine::reset() {
    heatingLockedUntilMs_ = 0;
    coolingLockedUntilMs_ = 0;
    heatingLimitedBySecondary_ = false;
    coolingLimitedBySecondary_ = false;
    status_ = Status{};
    status_.reason = "idle";
}

bool ControllerEngine::update(const FermentationConfig& config, const Inputs& inputs, OutputManager& outputs) {
    status_.automaticControlActive = false;
    status_.heatingDemand = false;
    status_.coolingDemand = false;

    if (!inputs.hasPrimaryTemp) {
        const bool outputsChanged = forceOutputsOff(outputs);
        const String reason = inputs.faultReason.isEmpty() ? "primary sensor invalid" : inputs.faultReason;
        return setState(State::Fault, reason) || outputsChanged;
    }

    if (config.mode == "manual") {
        if (config.manual.output == "heating") {
            return requestHeating(config, inputs, outputs, State::ManualHeating, "manual heating", "manual heating delay");
        }
        if (config.manual.output == "cooling") {
            return requestCooling(config, inputs, outputs, State::ManualCooling, "manual cooling", "manual cooling delay");
        }
        return requestOff(config, outputs, inputs, State::ManualOff, "manual off");
    }

    if (config.mode != "thermostat" && config.mode != "profile") {
        return requestOff(config, outputs, inputs, State::Idle, "mode inactive");
    }

    status_.automaticControlActive = true;

    const float setpointC = config.thermostat.setpointC;
    const float hysteresisC = config.thermostat.hysteresisC;
    const float heatThresholdC = setpointC - hysteresisC;
    const float coolThresholdC = setpointC + hysteresisC;
    const float processTempC = inputs.primaryTempC;

    if (processTempC <= heatThresholdC) {
        status_.heatingDemand = true;
    } else if (processTempC >= coolThresholdC) {
        status_.coolingDemand = true;
    }

    updateSecondaryLimits(config, inputs);

    if (status_.heatingDemand && heatingLimitedBySecondary_) {
        const bool changed = outputs.heatingState() == OutputState::On ? outputs.setHeating(OutputState::Off) : false;
        return setState(State::Idle, "heating limited by chamber") || changed;
    }

    if (status_.coolingDemand && coolingLimitedBySecondary_) {
        const bool changed = outputs.coolingState() == OutputState::On ? outputs.setCooling(OutputState::Off) : false;
        return setState(State::Idle, "cooling limited by chamber") || changed;
    }

    if (status_.heatingDemand) {
        return requestHeating(config, inputs, outputs, State::Heating, "heating", "heating delay");
    }

    if (status_.coolingDemand) {
        return requestCooling(config, inputs, outputs, State::Cooling, "cooling", "cooling delay");
    }

    return requestOff(config, outputs, inputs, State::Idle, "within hysteresis");
}

const ControllerEngine::Status& ControllerEngine::status() const {
    return status_;
}

bool ControllerEngine::forceOutputsOff(OutputManager& outputs) {
    const bool heatingChanged =
        outputs.heatingState() != OutputState::Off ? outputs.setHeating(OutputState::Off) : false;
    const bool coolingChanged =
        outputs.coolingState() != OutputState::Off ? outputs.setCooling(OutputState::Off) : false;
    return heatingChanged || coolingChanged;
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
        case State::ManualOff:
            return "manual_off";
        case State::ManualHeating:
            return "manual_heating";
        case State::ManualCooling:
            return "manual_cooling";
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

void ControllerEngine::updateSecondaryLimits(const FermentationConfig& config, const Inputs& inputs) {
    if (!config.sensors.secondaryEnabled || !inputs.hasSecondaryTemp) {
        heatingLimitedBySecondary_ = false;
        coolingLimitedBySecondary_ = false;
        return;
    }

    const float setpointC = config.thermostat.setpointC;
    const float hy2 = config.sensors.secondaryLimitHysteresisC;

    if (coolingLimitedBySecondary_) {
        if (inputs.secondaryTempC > (setpointC - (0.5f * hy2))) {
            coolingLimitedBySecondary_ = false;
        }
    } else if (inputs.secondaryTempC < (setpointC - hy2)) {
        coolingLimitedBySecondary_ = true;
    }

    if (heatingLimitedBySecondary_) {
        if (inputs.secondaryTempC < (setpointC + (0.5f * hy2))) {
            heatingLimitedBySecondary_ = false;
        }
    } else if (inputs.secondaryTempC > (setpointC + hy2)) {
        heatingLimitedBySecondary_ = true;
    }
}

bool ControllerEngine::requestHeating(
    const FermentationConfig& config,
    const Inputs& inputs,
    OutputManager& outputs,
    State activeState,
    const String& activeReason,
    const String& lockedReason) {
    const uint32_t remaining = remainingSeconds(heatingLockedUntilMs_, inputs.nowMs);
    if (remaining > 0) {
        bool changed = false;
        if (outputs.coolingState() == OutputState::On) {
            changed = outputs.setCooling(OutputState::Off);
            const uint32_t nextLockUntil =
                inputs.nowMs + static_cast<uint32_t>(config.thermostat.heatingDelaySeconds) * kMsPerSecond;
            if (static_cast<int32_t>(nextLockUntil - heatingLockedUntilMs_) > 0) {
                heatingLockedUntilMs_ = nextLockUntil;
            }
        }

        const uint32_t refreshedRemaining = remainingSeconds(heatingLockedUntilMs_, inputs.nowMs);
        return setState(State::LockedHeating, lockedReason + " " + String(refreshedRemaining) + "s") || changed;
    }

    const bool changed = outputs.setHeating(OutputState::On);
    outputs.setCooling(OutputState::Off);
    const bool stateChanged = setState(activeState, activeReason);
    if (changed) {
        coolingLockedUntilMs_ =
            inputs.nowMs + static_cast<uint32_t>(config.thermostat.coolingDelaySeconds) * kMsPerSecond;
    }
    return stateChanged || changed;
}

bool ControllerEngine::requestCooling(
    const FermentationConfig& config,
    const Inputs& inputs,
    OutputManager& outputs,
    State activeState,
    const String& activeReason,
    const String& lockedReason) {
    const uint32_t remaining = remainingSeconds(coolingLockedUntilMs_, inputs.nowMs);
    if (remaining > 0) {
        bool changed = false;
        if (outputs.heatingState() == OutputState::On) {
            changed = outputs.setHeating(OutputState::Off);
            const uint32_t nextLockUntil =
                inputs.nowMs + static_cast<uint32_t>(config.thermostat.coolingDelaySeconds) * kMsPerSecond;
            if (static_cast<int32_t>(nextLockUntil - coolingLockedUntilMs_) > 0) {
                coolingLockedUntilMs_ = nextLockUntil;
            }
        }

        const uint32_t refreshedRemaining = remainingSeconds(coolingLockedUntilMs_, inputs.nowMs);
        return setState(State::LockedCooling, lockedReason + " " + String(refreshedRemaining) + "s") || changed;
    }

    const bool changed = outputs.setCooling(OutputState::On);
    outputs.setHeating(OutputState::Off);
    const bool stateChanged = setState(activeState, activeReason);
    if (changed) {
        heatingLockedUntilMs_ =
            inputs.nowMs + static_cast<uint32_t>(config.thermostat.heatingDelaySeconds) * kMsPerSecond;
    }
    return stateChanged || changed;
}

bool ControllerEngine::requestOff(
    const FermentationConfig& config,
    OutputManager& outputs,
    const Inputs& inputs,
    State state,
    const String& reason) {
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

    const bool stateChanged = setState(state, reason);
    return stateChanged || heatingChanged || coolingChanged;
}
