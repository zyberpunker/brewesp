#include "output/OutputManager.h"

#include "output/GpioOutputDriver.h"
#include "output/KasaLocalDriver.h"
#include "output/ShellyHttpRpcDriver.h"

bool OutputManager::begin(const SystemConfig& config) {
    return applyConfig(config);
}

bool OutputManager::applyConfig(const SystemConfig& config) {
    heating_ = createDriver(config.heatingOutput);
    cooling_ = createDriver(config.coolingOutput);

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
}

void OutputManager::refreshStates() {
    update();
}

bool OutputManager::setHeating(OutputState state) {
    if (!heating_) {
        return false;
    }

    if (state == OutputState::On && cooling_ && cooling_->getState() == OutputState::On) {
        cooling_->setState(OutputState::Off);
    }

    return heating_->setState(state);
}

bool OutputManager::setCooling(OutputState state) {
    if (!cooling_) {
        return false;
    }

    if (state == OutputState::On && heating_ && heating_->getState() == OutputState::On) {
        heating_->setState(OutputState::Off);
    }

    return cooling_->setState(state);
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
