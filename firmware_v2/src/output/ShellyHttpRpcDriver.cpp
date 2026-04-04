#include "output/ShellyHttpRpcDriver.h"

ShellyHttpRpcDriver::ShellyHttpRpcDriver(String host, uint16_t port, uint8_t switchId)
    : host_(std::move(host)), port_(port), switchId_(switchId) {}

const char* ShellyHttpRpcDriver::driverName() const {
    return "shelly_http_rpc";
}

bool ShellyHttpRpcDriver::begin() {
    Serial.printf(
        "[shelly] init host=%s port=%u switch_id=%u\r\n",
        host_.c_str(),
        port_,
        switchId_);
    state_ = OutputState::Off;
    return true;
}

bool ShellyHttpRpcDriver::setState(OutputState state) {
    if (state == OutputState::Unknown) {
        return false;
    }

    Serial.printf(
        "[shelly] TODO set host=%s switch_id=%u state=%s\r\n",
        host_.c_str(),
        switchId_,
        state == OutputState::On ? "on" : "off");

    state_ = state;
    return true;
}

OutputState ShellyHttpRpcDriver::getState() const {
    return state_;
}

String ShellyHttpRpcDriver::describe() const {
    return "shelly(host=" + host_ + ", switch_id=" + String(switchId_) + ")";
}
