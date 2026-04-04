#pragma once

#include <Arduino.h>
#include <DallasTemperature.h>
#include <OneWire.h>

#include "config/SystemConfig.h"

class SensorManager {
public:
    static constexpr uint8_t kDefaultOneWireGpio = 32;

    struct Reading {
        bool present = false;
        bool valid = false;
        DeviceAddress address{};
        float tempC = 0.0f;
        uint32_t updatedAtMs = 0;
    };

    bool begin(const SystemConfig& config);
    void update(const SystemConfig& config);

    const Reading& beerProbe() const;
    const Reading& chamberProbe() const;
    bool hasAnySensor() const;
    String describeBeerProbe() const;
    String describeChamberProbe() const;
    String beerProbeRom() const;
    String chamberProbeRom() const;

private:
    struct ReadState {
        bool configured = false;
        DeviceAddress address{};
        Reading reading;
    };

    static bool parseRomString(const String& value, DeviceAddress address);
    static String formatAddress(const DeviceAddress address);
    static bool sameAddress(const DeviceAddress a, const DeviceAddress b);
    static bool isValidTemperature(float tempC);

    void assignProbeAddresses(const SystemConfig& config);
    void readProbe(ReadState& probe, uint32_t nowMs);

    bool busReady_ = false;
    bool oneWireInitialized_ = false;
    uint32_t lastPollMs_ = 0;
    uint8_t detectedCount_ = 0;
    OneWire oneWire_{kDefaultOneWireGpio};
    DallasTemperature sensors_{&oneWire_};
    ReadState beer_;
    ReadState chamber_;
};
