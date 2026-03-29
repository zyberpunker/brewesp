#pragma once

#include <Arduino.h>

#include <functional>

class WiFiUDP;

class KasaDiscovery {
public:
    using DeviceCallback = std::function<void(const String&)>;

    bool discover(DeviceCallback callback, uint32_t timeoutMs = 3000);

private:
    void sendDiscoveryPacket(WiFiUDP& udp, const uint8_t* packet, size_t length, const IPAddress& target) const;
    String buildDiscoveryRequest() const;
    String decryptPacket(const uint8_t* data, size_t length) const;
    String escapeJson(const String& value) const;
    String parseJsonString(const String& payload, const char* key) const;
    int parseJsonInt(const String& payload, const char* key, int fallback) const;
};
