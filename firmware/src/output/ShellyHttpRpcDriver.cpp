#include "output/ShellyHttpRpcDriver.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClient.h>

namespace {
constexpr uint32_t kHttpTimeoutMs = 3000;
constexpr uint32_t kVerifyDelayMs = 150;
constexpr uint32_t kPollIntervalMs = 30000;
}

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
    state_ = OutputState::Unknown;
    if (!host_.isEmpty() && WiFi.status() == WL_CONNECTED) {
        queryState("boot");
    }
    return true;
}

bool ShellyHttpRpcDriver::setState(OutputState state) {
    if (state == OutputState::Unknown || host_.isEmpty() || WiFi.status() != WL_CONNECTED) {
        return false;
    }

    const char* desiredState = state == OutputState::On ? "true" : "false";
    Serial.printf(
        "[shelly] setState request host=%s switch_id=%u desired=%s current=%s\r\n",
        host_.c_str(),
        switchId_,
        state == OutputState::On ? "on" : "off",
        state_ == OutputState::On ? "on" : state_ == OutputState::Off ? "off" : "unknown");

    String response;
    if (!performGet("/rpc/Switch.Set?id=" + String(switchId_) + "&on=" + String(desiredState), response)) {
        Serial.printf("[shelly] setState failed host=%s\r\n", host_.c_str());
        state_ = OutputState::Unknown;
        return false;
    }

    String rpcError;
    if (responseHasRpcError(response, rpcError)) {
        Serial.printf(
            "[shelly] setState rpc error host=%s switch_id=%u error=%s response=%s\r\n",
            host_.c_str(),
            switchId_,
            rpcError.c_str(),
            response.c_str());
        state_ = OutputState::Unknown;
        return false;
    }

    delay(kVerifyDelayMs);
    if (!queryState("verify")) {
        Serial.printf("[shelly] setState verify failed host=%s\r\n", host_.c_str());
        state_ = OutputState::Unknown;
        return false;
    }

    if (state_ != state) {
        Serial.printf(
            "[shelly] setState mismatch host=%s switch_id=%u expected=%s actual=%s\r\n",
            host_.c_str(),
            switchId_,
            state == OutputState::On ? "on" : "off",
            state_ == OutputState::On ? "on" : state_ == OutputState::Off ? "off" : "unknown");
        return false;
    }

    Serial.printf(
        "[shelly] host=%s switch_id=%u state=%s\r\n",
        host_.c_str(),
        switchId_,
        state == OutputState::On ? "on" : "off");
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

void ShellyHttpRpcDriver::update() {
    if (host_.isEmpty() || WiFi.status() != WL_CONNECTED) {
        return;
    }

    const uint32_t now = millis();
    if (now - lastPollMs_ < kPollIntervalMs) {
        return;
    }

    lastPollMs_ = now;
    queryState();
}

bool ShellyHttpRpcDriver::queryState(const char* context) {
    String response;
    if (!performGet("/rpc/Switch.GetStatus?id=" + String(switchId_), response)) {
        Serial.printf("[shelly] queryState failed host=%s\r\n", host_.c_str());
        return false;
    }

    String rpcError;
    if (responseHasRpcError(response, rpcError)) {
        Serial.printf(
            "[shelly] queryState rpc error host=%s switch_id=%u error=%s response=%s\r\n",
            host_.c_str(),
            switchId_,
            rpcError.c_str(),
            response.c_str());
        return false;
    }

    OutputState parsed = OutputState::Unknown;
    if (!parseOutputState(response, parsed) || parsed == OutputState::Unknown) {
        Serial.printf(
            "[shelly] could not parse state host=%s switch_id=%u response=%s\r\n",
            host_.c_str(),
            switchId_,
            response.c_str());
        return false;
    }

    state_ = parsed;
    lastPollMs_ = millis();
    if (context != nullptr) {
        Serial.printf(
            "[shelly] queryState context=%s host=%s switch_id=%u parsed=%s response=%s\r\n",
            context,
            host_.c_str(),
            switchId_,
            state_ == OutputState::On ? "on" : "off",
            response.c_str());
    }
    return true;
}

bool ShellyHttpRpcDriver::performGet(const String& path, String& response) {
    HTTPClient http;
    WiFiClient client;
    client.setTimeout(kHttpTimeoutMs);

    const String url = buildUrl(path);
    if (!http.begin(client, url)) {
        Serial.printf("[shelly] begin failed url=%s\r\n", url.c_str());
        return false;
    }

    http.setTimeout(kHttpTimeoutMs);
    const int statusCode = http.GET();
    if (statusCode != HTTP_CODE_OK) {
        response = http.getString();
        Serial.printf(
            "[shelly] http status=%d url=%s response=%s\r\n",
            statusCode,
            url.c_str(),
            response.c_str());
        http.end();
        return false;
    }

    response = http.getString();
    http.end();
    return true;
}

bool ShellyHttpRpcDriver::parseOutputState(const String& response, OutputState& state) const {
    StaticJsonDocument<512> doc;
    const DeserializationError error = deserializeJson(doc, response);
    if (error) {
        return false;
    }

    JsonVariantConst output = doc["output"];
    if (output.isNull()) {
        output = doc["result"]["output"];
    }
    if (output.isNull()) {
        output = doc["params"]["output"];
    }

    if (!output.is<bool>()) {
        return false;
    }

    state = output.as<bool>() ? OutputState::On : OutputState::Off;
    return true;
}

bool ShellyHttpRpcDriver::responseHasRpcError(const String& response, String& message) const {
    StaticJsonDocument<512> doc;
    const DeserializationError error = deserializeJson(doc, response);
    if (error) {
        return false;
    }

    JsonVariantConst rpcError = doc["error"];
    if (rpcError.isNull()) {
        return false;
    }

    const char* text = rpcError["message"] | rpcError["msg"] | "unknown";
    message = String(text);
    return true;
}

String ShellyHttpRpcDriver::buildUrl(const String& path) const {
    return "http://" + host_ + ":" + String(port_) + path;
}
