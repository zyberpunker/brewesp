#pragma once

#include <Arduino.h>

#include <functional>

class ShellyDiscovery {
public:
    using DeviceCallback = std::function<void(const String&)>;

    bool discover(DeviceCallback callback, uint32_t timeoutMs = 15000);

private:
    String escapeJson(const String& value) const;
    bool probeHost(const String& host, DeviceCallback callback, String* failureReason = nullptr) const;
};
