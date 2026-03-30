#include "sensor/SensorManager.h"

#include <string.h>

namespace {
constexpr float kDisconnectedTempC = DEVICE_DISCONNECTED_C;
constexpr uint8_t kSensorResolutionBits = 10;
constexpr uint8_t kMaxProbeCount = 8;
}

bool SensorManager::begin(const SystemConfig& config) {
    oneWire_.begin(config.sensors.oneWireGpio);
    oneWireInitialized_ = true;
    sensors_.begin();
    sensors_.setResolution(kSensorResolutionBits);
    sensors_.setWaitForConversion(true);
    assignProbeAddresses(config);
    busReady_ = true;
    lastPollMs_ = 0;
    return true;
}

void SensorManager::update(const SystemConfig& config) {
    if (!oneWireInitialized_) {
        begin(config);
    }

    const uint32_t nowMs = millis();
    const uint32_t pollIntervalMs = config.sensors.pollIntervalSeconds * 1000UL;
    if (lastPollMs_ != 0 && nowMs - lastPollMs_ < pollIntervalMs) {
        return;
    }

    assignProbeAddresses(config);
    sensors_.requestTemperatures();
    readProbe(beer_, nowMs);
    readProbe(chamber_, nowMs);
    lastPollMs_ = nowMs;
}

const SensorManager::Reading& SensorManager::beerProbe() const {
    return beer_.reading;
}

const SensorManager::Reading& SensorManager::chamberProbe() const {
    return chamber_.reading;
}

bool SensorManager::hasAnySensor() const {
    return beer_.reading.present || chamber_.reading.present;
}

String SensorManager::describeBeerProbe() const {
    if (!beer_.reading.present) {
        return "beer:missing";
    }
    return "beer:" + formatAddress(beer_.reading.address);
}

String SensorManager::describeChamberProbe() const {
    if (!chamber_.reading.present) {
        return "chamber:missing";
    }
    return "chamber:" + formatAddress(chamber_.reading.address);
}

String SensorManager::beerProbeRom() const {
    if (!beer_.reading.present) {
        return String();
    }
    return formatAddress(beer_.reading.address);
}

String SensorManager::chamberProbeRom() const {
    if (!chamber_.reading.present) {
        return String();
    }
    return formatAddress(chamber_.reading.address);
}

bool SensorManager::parseRomString(const String& value, DeviceAddress address) {
    if (value.length() != 16) {
        return false;
    }

    for (uint8_t i = 0; i < 8; ++i) {
        const char high = value[i * 2];
        const char low = value[i * 2 + 1];
        char buffer[3] = {high, low, '\0'};
        char* end = nullptr;
        const long parsed = strtol(buffer, &end, 16);
        if (end == buffer || *end != '\0' || parsed < 0 || parsed > 255) {
            return false;
        }
        address[i] = static_cast<uint8_t>(parsed);
    }
    return true;
}

String SensorManager::formatAddress(const DeviceAddress address) {
    char buffer[17];
    for (uint8_t i = 0; i < 8; ++i) {
        snprintf(buffer + (i * 2), sizeof(buffer) - (i * 2), "%02X", address[i]);
    }
    buffer[16] = '\0';
    return String(buffer);
}

bool SensorManager::sameAddress(const DeviceAddress a, const DeviceAddress b) {
    return memcmp(a, b, sizeof(DeviceAddress)) == 0;
}

bool SensorManager::isValidTemperature(float tempC) {
    return tempC != kDisconnectedTempC && tempC > -55.0f && tempC < 125.0f;
}

void SensorManager::assignProbeAddresses(const SystemConfig& config) {
    detectedCount_ = static_cast<uint8_t>(sensors_.getDeviceCount());

    DeviceAddress knownAddresses[kMaxProbeCount];
    uint8_t knownCount = 0;
    for (uint8_t index = 0; index < detectedCount_ && index < kMaxProbeCount; ++index) {
        if (sensors_.getAddress(knownAddresses[knownCount], index)) {
            ++knownCount;
        }
    }

    beer_.configured = false;
    chamber_.configured = false;

    if (parseRomString(config.sensors.beerProbeRom, beer_.address)) {
        beer_.configured = true;
    }
    if (parseRomString(config.sensors.chamberProbeRom, chamber_.address)) {
        chamber_.configured = true;
    }

    if (!beer_.configured && knownCount >= 1) {
        memcpy(beer_.address, knownAddresses[0], sizeof(DeviceAddress));
        beer_.configured = true;
    }

    if (!chamber_.configured && knownCount >= 2) {
        uint8_t chamberIndex = 1;
        if (beer_.configured && sameAddress(knownAddresses[1], beer_.address) && knownCount >= 3) {
            chamberIndex = 2;
        }
        memcpy(chamber_.address, knownAddresses[chamberIndex], sizeof(DeviceAddress));
        chamber_.configured = true;
    }
}

void SensorManager::readProbe(ReadState& probe, uint32_t nowMs) {
    probe.reading.present = false;
    probe.reading.valid = false;

    if (!probe.configured) {
        return;
    }

    if (!sensors_.isConnected(probe.address)) {
        return;
    }

    const float tempC = sensors_.getTempC(probe.address);
    probe.reading.present = true;
    memcpy(probe.reading.address, probe.address, sizeof(DeviceAddress));
    if (!isValidTemperature(tempC)) {
        return;
    }

    probe.reading.valid = true;
    probe.reading.tempC = tempC;
    probe.reading.updatedAtMs = nowMs;
}
