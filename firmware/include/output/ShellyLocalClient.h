#pragma once

#include <Arduino.h>

struct ShellyDeviceInfo {
    String deviceId;
    String model;
    String type;
    int generation = 0;
    bool authEnabled = false;
};

struct ShellyRelayStatus {
    bool isOn = false;
    String name;
};

class ShellyLocalClient {
public:
    static bool getDeviceInfo(
        const String& host,
        uint16_t port,
        ShellyDeviceInfo& info,
        String* errorReason = nullptr);
    static bool getRelayStatus(
        const String& host,
        uint16_t port,
        uint8_t switchId,
        ShellyRelayStatus& status,
        String* errorReason = nullptr);
    static bool setRelayState(const String& host, uint16_t port, uint8_t switchId, bool on);

private:
    static bool sendGet(
        const String& host,
        uint16_t port,
        const String& path,
        String& body,
        String* errorReason = nullptr);
    static bool parseRpcRelayStatus(const String& body, ShellyRelayStatus& status);
    static bool parseCompatRelayStatus(const String& body, ShellyRelayStatus& status);
};
