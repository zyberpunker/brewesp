#include "output/KasaLocalDriver.h"

#include <WiFi.h>
#include <WiFiClient.h>

#include <memory>

namespace {
const uint8_t kLegacyInitialKey = 171;
const uint32_t kSocketTimeoutMs = 1500;
}

KasaLocalDriver::KasaLocalDriver(String host, uint16_t port, String alias, uint32_t pollIntervalSeconds)
    : host_(std::move(host)),
      port_(port),
      alias_(std::move(alias)),
      pollIntervalSeconds_(pollIntervalSeconds) {}

const char* KasaLocalDriver::driverName() const {
    return "kasa_local";
}

bool KasaLocalDriver::begin() {
    Serial.printf(
        "[kasa] init host=%s port=%u alias=%s poll_interval_s=%lu\r\n",
        host_.c_str(),
        port_,
        alias_.c_str(),
        static_cast<unsigned long>(pollIntervalSeconds_));

    state_ = OutputState::Unknown;
    if (WiFi.status() == WL_CONNECTED) {
        queryState();
    }
    return true;
}

bool KasaLocalDriver::setState(OutputState state) {
    if (state == OutputState::Unknown || host_.isEmpty() || WiFi.status() != WL_CONNECTED) {
        return false;
    }

    Serial.printf(
        "[kasa] setState request host=%s desired=%s current=%s\r\n",
        host_.c_str(),
        state == OutputState::On ? "on" : "off",
        state_ == OutputState::On ? "on" : state_ == OutputState::Off ? "off" : "unknown");

    const char* relayStateValue = state == OutputState::On ? "1" : "0";
    const String payload =
        "{\"system\":{\"set_relay_state\":{\"state\":" + String(relayStateValue) + "}}}";
    String response;
    if (!sendRequest(payload, response)) {
        Serial.printf("[kasa] setState failed host=%s\r\n", host_.c_str());
        return false;
    }

    Serial.printf("[kasa] setState raw response host=%s response=%s\r\n", host_.c_str(), response.c_str());

    const int errCode = parseErrCode(response);
    if (errCode != 0) {
        Serial.printf(
            "[kasa] setState err_code=%d host=%s response=%s\r\n",
            errCode,
            host_.c_str(),
            response.c_str());
        state_ = OutputState::Unknown;
        return false;
    }

    delay(150);
    if (!queryState("verify")) {
        Serial.printf("[kasa] setState verify failed host=%s\r\n", host_.c_str());
        state_ = OutputState::Unknown;
        return false;
    }

    if (state_ != state) {
        Serial.printf(
            "[kasa] setState mismatch host=%s expected=%s actual=%s\r\n",
            host_.c_str(),
            state == OutputState::On ? "on" : "off",
            state_ == OutputState::On ? "on" : state_ == OutputState::Off ? "off" : "unknown");
        return false;
    }

    Serial.printf(
        "[kasa] host=%s alias=%s state=%s\r\n",
        host_.c_str(),
        alias_.c_str(),
        state == OutputState::On ? "on" : "off");
    return true;
}

OutputState KasaLocalDriver::getState() const {
    return state_;
}

String KasaLocalDriver::describe() const {
    const char* stateName = state_ == OutputState::On
        ? "on"
        : state_ == OutputState::Off ? "off"
                                     : "unknown";
    return "kasa(host=" + host_ + ", alias=" + alias_ + ", state=" + stateName + ")";
}

void KasaLocalDriver::update() {
    if (pollIntervalSeconds_ == 0 || host_.isEmpty() || WiFi.status() != WL_CONNECTED) {
        return;
    }

    const uint32_t now = millis();
    if (now - lastPollMs_ < pollIntervalSeconds_ * 1000UL) {
        return;
    }

    lastPollMs_ = now;
    queryState();
}

bool KasaLocalDriver::queryState(const char* context) {
    String response;
    if (!sendRequest("{\"system\":{\"get_sysinfo\":{}}}", response)) {
        Serial.printf("[kasa] queryState failed host=%s\r\n", host_.c_str());
        return false;
    }

    const int errCode = parseErrCode(response);
    if (errCode != 0) {
        Serial.printf(
            "[kasa] queryState err_code=%d host=%s response=%s\r\n",
            errCode,
            host_.c_str(),
            response.c_str());
        return false;
    }

    const OutputState parsed = parseRelayState(response);
    if (parsed == OutputState::Unknown) {
        Serial.printf(
            "[kasa] could not parse relay state host=%s response=%s\r\n",
            host_.c_str(),
            response.c_str());
        return false;
    }

    state_ = parsed;
    if (context != nullptr) {
        Serial.printf(
            "[kasa] queryState context=%s host=%s parsed=%s response=%s\r\n",
            context,
            host_.c_str(),
            state_ == OutputState::On ? "on" : "off",
            response.c_str());
    }
    lastPollMs_ = millis();
    return true;
}

bool KasaLocalDriver::sendRequest(const String& payload, String& response) {
    WiFiClient client;
    client.setTimeout(kSocketTimeoutMs);

    if (!client.connect(host_.c_str(), port_)) {
        Serial.printf("[kasa] connect failed host=%s port=%u\r\n", host_.c_str(), port_);
        return false;
    }

    const size_t payloadLength = payload.length();
    std::unique_ptr<uint8_t[]> packet(new uint8_t[payloadLength + 4]);
    packet[0] = static_cast<uint8_t>((payloadLength >> 24) & 0xFF);
    packet[1] = static_cast<uint8_t>((payloadLength >> 16) & 0xFF);
    packet[2] = static_cast<uint8_t>((payloadLength >> 8) & 0xFF);
    packet[3] = static_cast<uint8_t>(payloadLength & 0xFF);

    uint8_t key = kLegacyInitialKey;
    for (size_t i = 0; i < payloadLength; ++i) {
        const uint8_t encrypted = static_cast<uint8_t>(payload[i]) ^ key;
        key = encrypted;
        packet[i + 4] = encrypted;
    }

    if (client.write(packet.get(), payloadLength + 4) != payloadLength + 4) {
        client.stop();
        return false;
    }

    uint8_t header[4];
    if (client.readBytes(header, sizeof(header)) != sizeof(header)) {
        client.stop();
        return false;
    }

    const uint32_t responseLength =
        (static_cast<uint32_t>(header[0]) << 24) | (static_cast<uint32_t>(header[1]) << 16)
        | (static_cast<uint32_t>(header[2]) << 8) | static_cast<uint32_t>(header[3]);

    std::unique_ptr<uint8_t[]> responseBuffer(new uint8_t[responseLength]);
    if (client.readBytes(responseBuffer.get(), responseLength) != responseLength) {
        client.stop();
        return false;
    }

    key = kLegacyInitialKey;
    response.reserve(responseLength);
    for (uint32_t i = 0; i < responseLength; ++i) {
        const uint8_t encrypted = responseBuffer[i];
        const uint8_t plain = encrypted ^ key;
        key = encrypted;
        response += static_cast<char>(plain);
    }

    client.stop();
    return true;
}

OutputState KasaLocalDriver::parseRelayState(const String& response) const {
    int relayIndex = response.indexOf("\"relay_state\"");
    if (relayIndex < 0) {
        return OutputState::Unknown;
    }

    relayIndex = response.indexOf(':', relayIndex);
    if (relayIndex < 0) {
        return OutputState::Unknown;
    }

    ++relayIndex;
    while (relayIndex < response.length() && response[relayIndex] == ' ') {
        ++relayIndex;
    }

    if (relayIndex >= response.length()) {
        return OutputState::Unknown;
    }

    if (response[relayIndex] == '1') {
        return OutputState::On;
    }
    if (response[relayIndex] == '0') {
        return OutputState::Off;
    }
    return OutputState::Unknown;
}

int KasaLocalDriver::parseErrCode(const String& response) const {
    int errIndex = response.indexOf("\"err_code\"");
    if (errIndex < 0) {
        return 0;
    }

    errIndex = response.indexOf(':', errIndex);
    if (errIndex < 0) {
        return 0;
    }

    ++errIndex;
    while (errIndex < response.length() && response[errIndex] == ' ') {
        ++errIndex;
    }

    int endIndex = errIndex;
    if (endIndex < response.length() && response[endIndex] == '-') {
        ++endIndex;
    }
    while (endIndex < response.length() && isDigit(response[endIndex])) {
        ++endIndex;
    }

    if (endIndex == errIndex) {
        return 0;
    }

    return response.substring(errIndex, endIndex).toInt();
}
