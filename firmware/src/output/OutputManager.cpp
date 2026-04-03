#include "output/OutputManager.h"

#include "output/GpioOutputDriver.h"
#include "output/KasaLocalDriver.h"
#include "output/ShellyHttpRpcDriver.h"
#include "support/Logger.h"

namespace {
constexpr uint32_t kOutputRetryIntervalMs = 15000UL;

const char* outputStateName(OutputState state) {
    switch (state) {
        case OutputState::On:
            return "on";
        case OutputState::Off:
            return "off";
        case OutputState::Unknown:
        default:
            return "unknown";
    }
}
}

bool OutputManager::begin(const SystemConfig& config) {
    return applyConfig(config);
}

bool OutputManager::applyConfig(const SystemConfig& config) {
    heating_ = createDriver(config.heatingOutput);
    cooling_ = createDriver(config.coolingOutput);
    desiredHeatingState_ = OutputState::Unknown;
    desiredCoolingState_ = OutputState::Unknown;
    nextHeatingAttemptAtMs_ = 0;
    nextCoolingAttemptAtMs_ = 0;

    const bool heatingOk = heating_ ? heating_->begin() : false;
    const bool coolingOk = cooling_ ? cooling_->begin() : false;

    return heatingOk && coolingOk;
}

void OutputManager::update() {
    if (heating_) {
        heating_->update();
    }
    if (cooling_) {
        cooling_->update();
    }

    const uint32_t nowMs = millis();
    applyCoolingIfDue(nowMs);
    applyHeatingIfDue(nowMs);
}

void OutputManager::refreshStates() {
    update();
}

bool OutputManager::setHeating(OutputState state) {
    if (!heating_ || state == OutputState::Unknown) {
        return false;
    }

    const bool changed = desiredHeatingState_ != state;
    desiredHeatingState_ = state;
    if (changed) {
        nextHeatingAttemptAtMs_ = 0;
    }

    if (state == OutputState::On && cooling_) {
        desiredCoolingState_ = OutputState::Off;
        nextCoolingAttemptAtMs_ = 0;
    }

    return changed;
}

bool OutputManager::setCooling(OutputState state) {
    if (!cooling_ || state == OutputState::Unknown) {
        return false;
    }

    const bool changed = desiredCoolingState_ != state;
    desiredCoolingState_ = state;
    if (changed) {
        nextCoolingAttemptAtMs_ = 0;
    }

    if (state == OutputState::On && heating_) {
        desiredHeatingState_ = OutputState::Off;
        nextHeatingAttemptAtMs_ = 0;
    }

    return changed;
}

OutputState OutputManager::heatingState() const {
    return heating_ ? heating_->getState() : OutputState::Unknown;
}

OutputState OutputManager::coolingState() const {
    return cooling_ ? cooling_->getState() : OutputState::Unknown;
}

String OutputManager::describeHeating() const {
    return heating_ ? heating_->describe() : "missing";
}

String OutputManager::describeCooling() const {
    return cooling_ ? cooling_->describe() : "missing";
}

bool OutputManager::applyHeatingIfDue(uint32_t nowMs) {
    if (!heating_ || desiredHeatingState_ == OutputState::Unknown) {
        return false;
    }

    if (heating_->getState() == desiredHeatingState_) {
        return false;
    }

    if (nextHeatingAttemptAtMs_ != 0 && static_cast<int32_t>(nowMs - nextHeatingAttemptAtMs_) < 0) {
        return false;
    }

    if (desiredHeatingState_ == OutputState::On && cooling_ && cooling_->getState() != OutputState::Off) {
        return false;
    }

    const bool applied = heating_->setState(desiredHeatingState_);
    if (applied) {
        nextHeatingAttemptAtMs_ = 0;
        LOG_DEBUG(
            "[outputs] heating applied desired=%s actual=%s\r\n",
            outputStateName(desiredHeatingState_),
            outputStateName(heating_->getState()));
    } else {
        nextHeatingAttemptAtMs_ = nowMs + kOutputRetryIntervalMs;
        LOG_DEBUG(
            "[outputs] heating apply failed desired=%s retry_in_ms=%lu\r\n",
            outputStateName(desiredHeatingState_),
            static_cast<unsigned long>(kOutputRetryIntervalMs));
    }
    return applied;
}

bool OutputManager::applyCoolingIfDue(uint32_t nowMs) {
    if (!cooling_ || desiredCoolingState_ == OutputState::Unknown) {
        return false;
    }

    if (cooling_->getState() == desiredCoolingState_) {
        return false;
    }

    if (nextCoolingAttemptAtMs_ != 0 && static_cast<int32_t>(nowMs - nextCoolingAttemptAtMs_) < 0) {
        return false;
    }

    if (desiredCoolingState_ == OutputState::On && heating_ && heating_->getState() != OutputState::Off) {
        return false;
    }

    const bool applied = cooling_->setState(desiredCoolingState_);
    if (applied) {
        nextCoolingAttemptAtMs_ = 0;
        LOG_DEBUG(
            "[outputs] cooling applied desired=%s actual=%s\r\n",
            outputStateName(desiredCoolingState_),
            outputStateName(cooling_->getState()));
    } else {
        nextCoolingAttemptAtMs_ = nowMs + kOutputRetryIntervalMs;
        LOG_DEBUG(
            "[outputs] cooling apply failed desired=%s retry_in_ms=%lu\r\n",
            outputStateName(desiredCoolingState_),
            static_cast<unsigned long>(kOutputRetryIntervalMs));
    }
    return applied;
}

std::unique_ptr<OutputDriver> OutputManager::createDriver(const OutputConfig& config) {
    switch (config.driver) {
        case OutputDriverType::Gpio:
            if (config.pin < 0) {
                return nullptr;
            }
            return std::unique_ptr<OutputDriver>(
                new GpioOutputDriver(static_cast<uint8_t>(config.pin), config.activeHigh));
        case OutputDriverType::ShellyHttpRpc:
            return std::unique_ptr<OutputDriver>(
                new ShellyHttpRpcDriver(config.host, config.port, config.switchId));
        case OutputDriverType::KasaLocal:
            return std::unique_ptr<OutputDriver>(
                new KasaLocalDriver(config.host, config.port, config.alias, config.pollIntervalSeconds));
        case OutputDriverType::GenericMqttRelay:
        case OutputDriverType::None:
        default:
            return nullptr;
    }
}
