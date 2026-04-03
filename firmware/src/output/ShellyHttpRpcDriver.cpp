#include "output/ShellyHttpRpcDriver.h"

#include <WiFi.h>

#include "output/ShellyLocalClient.h"
#include "support/Logger.h"

namespace {
constexpr uint32_t kPollIntervalSeconds = 30;
}

ShellyHttpRpcDriver::ShellyHttpRpcDriver(String host, uint16_t port, uint8_t switchId)
    : host_(std::move(host)), port_(port), switchId_(switchId) {}

const char* ShellyHttpRpcDriver::driverName() const {
    return "shelly_http_rpc";
}

bool ShellyHttpRpcDriver::begin() {
    LOG_DEBUG(
        "[shelly] init host=%s port=%u switch_id=%u\r\n",
        host_.c_str(),
        port_,
        switchId_);
    state_ = OutputState::Unknown;
    if (WiFi.status() == WL_CONNECTED) {
        queryState();
    }
    return true;
}

bool ShellyHttpRpcDriver::setState(OutputState state) {
    if (state == OutputState::Unknown || host_.isEmpty() || WiFi.status() != WL_CONNECTED) {
        return false;
    }

    LOG_DEBUG(
        "[shelly] setState request host=%s switch_id=%u desired=%s current=%s\r\n",
        host_.c_str(),
        switchId_,
        state == OutputState::On ? "on" : "off",
        state_ == OutputState::On ? "on" : state_ == OutputState::Off ? "off" : "unknown");

    if (!ShellyLocalClient::setRelayState(host_, port_, switchId_, state == OutputState::On)) {
        LOG_DEBUG("[shelly] setState failed host=%s switch_id=%u\r\n", host_.c_str(), switchId_);
        state_ = OutputState::Unknown;
        return false;
    }

    delay(150);
    if (!queryState("verify")) {
        LOG_DEBUG("[shelly] setState verify failed host=%s switch_id=%u\r\n", host_.c_str(), switchId_);
        state_ = OutputState::Unknown;
        return false;
    }

    if (state_ != state) {
        LOG_WARN(
            "[shelly] setState mismatch host=%s switch_id=%u expected=%s actual=%s\r\n",
            host_.c_str(),
            switchId_,
            state == OutputState::On ? "on" : "off",
            state_ == OutputState::On ? "on" : state_ == OutputState::Off ? "off" : "unknown");
        return false;
    }

    return true;
}

OutputState ShellyHttpRpcDriver::getState() const {
    return state_;
}

String ShellyHttpRpcDriver::describe() const {
    const char* stateName = state_ == OutputState::On
        ? "on"
        : state_ == OutputState::Off ? "off"
                                     : "unknown";
    return "shelly(host=" + host_ + ", switch_id=" + String(switchId_) + ", state=" + stateName + ")";
}

bool ShellyHttpRpcDriver::queryState(const char* context) {
    ShellyRelayStatus status;
    if (!ShellyLocalClient::getRelayStatus(host_, port_, switchId_, status)) {
        return false;
    }

    state_ = status.isOn ? OutputState::On : OutputState::Off;
    if (context != nullptr) {
        LOG_DEBUG(
            "[shelly] queryState context=%s host=%s switch_id=%u parsed=%s\r\n",
            context,
            host_.c_str(),
            switchId_,
            state_ == OutputState::On ? "on" : "off");
    }
    lastPollMs_ = millis();
    return true;
}

void ShellyHttpRpcDriver::update() {
    if (host_.isEmpty() || WiFi.status() != WL_CONNECTED) {
        return;
    }

    const uint32_t now = millis();
    if (now - lastPollMs_ < kPollIntervalSeconds * 1000UL) {
        return;
    }

    lastPollMs_ = now;
    queryState();
}
