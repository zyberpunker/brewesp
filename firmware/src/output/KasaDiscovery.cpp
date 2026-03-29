#include "output/KasaDiscovery.h"

#include <WiFi.h>
#include <WiFiUdp.h>

#include <memory>

namespace {
const uint16_t kKasaPort = 9999;
const uint8_t kLegacyInitialKey = 171;
}

bool KasaDiscovery::discover(DeviceCallback callback, uint32_t timeoutMs) {
    if (WiFi.status() != WL_CONNECTED) {
        return false;
    }

    WiFiUDP udp;
    if (!udp.begin(0)) {
        return false;
    }

    const String request = buildDiscoveryRequest();
    const size_t payloadLength = request.length();
    std::unique_ptr<uint8_t[]> packet(new uint8_t[payloadLength + 4]);
    packet[0] = static_cast<uint8_t>((payloadLength >> 24) & 0xFF);
    packet[1] = static_cast<uint8_t>((payloadLength >> 16) & 0xFF);
    packet[2] = static_cast<uint8_t>((payloadLength >> 8) & 0xFF);
    packet[3] = static_cast<uint8_t>(payloadLength & 0xFF);

    uint8_t key = kLegacyInitialKey;
    for (size_t i = 0; i < payloadLength; ++i) {
        const uint8_t encrypted = static_cast<uint8_t>(request[i]) ^ key;
        key = encrypted;
        packet[i + 4] = encrypted;
    }

    sendDiscoveryPacket(udp, packet.get(), payloadLength + 4, IPAddress(255, 255, 255, 255));

    const IPAddress localIp = WiFi.localIP();
    const IPAddress subnetMask = WiFi.subnetMask();
    const IPAddress network(
        localIp[0] & subnetMask[0],
        localIp[1] & subnetMask[1],
        localIp[2] & subnetMask[2],
        localIp[3] & subnetMask[3]);
    const IPAddress broadcast(
        network[0] | static_cast<uint8_t>(~subnetMask[0]),
        network[1] | static_cast<uint8_t>(~subnetMask[1]),
        network[2] | static_cast<uint8_t>(~subnetMask[2]),
        network[3] | static_cast<uint8_t>(~subnetMask[3]));
    sendDiscoveryPacket(udp, packet.get(), payloadLength + 4, broadcast);

    for (uint16_t host = 1; host < 255; ++host) {
        IPAddress candidate(network[0], network[1], network[2], host);
        if (candidate == localIp || candidate == network || candidate == broadcast) {
            continue;
        }
        sendDiscoveryPacket(udp, packet.get(), payloadLength + 4, candidate);
        delay(3);
    }

    bool found = false;
    String seenHosts = ";";
    const uint32_t startedAt = millis();
    while (millis() - startedAt < timeoutMs) {
        const int packetSize = udp.parsePacket();
        if (packetSize <= 4) {
            delay(25);
            continue;
        }

        std::unique_ptr<uint8_t[]> response(new uint8_t[packetSize]);
        const int bytesRead = udp.read(response.get(), packetSize);
        if (bytesRead <= 4) {
            continue;
        }

        const String host = udp.remoteIP().toString();
        if (seenHosts.indexOf(";" + host + ";") >= 0) {
            continue;
        }
        seenHosts += host + ";";

        const String decrypted = decryptPacket(response.get() + 4, bytesRead - 4);
        const String alias = parseJsonString(decrypted, "alias");
        const String model = parseJsonString(decrypted, "model");
        const int relayState = parseJsonInt(decrypted, "relay_state", -1);

        String payload;
        payload.reserve(256);
        payload += "{\"driver\":\"kasa_local\"";
        payload += ",\"host\":\"" + escapeJson(host) + "\"";
        payload += ",\"port\":9999";
        payload += ",\"alias\":\"" + escapeJson(alias) + "\"";
        payload += ",\"model\":\"" + escapeJson(model) + "\"";
        if (relayState >= 0) {
            payload += ",\"is_on\":";
            payload += relayState == 1 ? "true" : "false";
        }
        payload += "}";
        callback(payload);
        found = true;
    }

    udp.stop();
    return found;
}

void KasaDiscovery::sendDiscoveryPacket(
    WiFiUDP& udp,
    const uint8_t* packet,
    size_t length,
    const IPAddress& target) const {
    udp.beginPacket(target, kKasaPort);
    udp.write(packet, length);
    udp.endPacket();
}

String KasaDiscovery::buildDiscoveryRequest() const {
    return "{\"system\":{\"get_sysinfo\":{}}}";
}

String KasaDiscovery::decryptPacket(const uint8_t* data, size_t length) const {
    String decrypted;
    decrypted.reserve(length);

    uint8_t key = kLegacyInitialKey;
    for (size_t i = 0; i < length; ++i) {
        const uint8_t encrypted = data[i];
        const uint8_t plain = encrypted ^ key;
        key = encrypted;
        decrypted += static_cast<char>(plain);
    }
    return decrypted;
}

String KasaDiscovery::escapeJson(const String& value) const {
    String escaped;
    escaped.reserve(value.length() + 8);
    for (size_t index = 0; index < value.length(); ++index) {
        const char ch = value[index];
        if (ch == '\\' || ch == '"') {
            escaped += '\\';
        }
        escaped += ch;
    }
    return escaped;
}

String KasaDiscovery::parseJsonString(const String& payload, const char* key) const {
    const String quotedKey = "\"" + String(key) + "\"";
    int keyIndex = payload.indexOf(quotedKey);
    if (keyIndex < 0) {
        return "";
    }

    int colonIndex = payload.indexOf(':', keyIndex + quotedKey.length());
    if (colonIndex < 0) {
        return "";
    }

    int valueStart = payload.indexOf('"', colonIndex + 1);
    if (valueStart < 0) {
        return "";
    }
    ++valueStart;

    String result;
    bool escaping = false;
    for (int i = valueStart; i < payload.length(); ++i) {
        const char ch = payload[i];
        if (escaping) {
            result += ch;
            escaping = false;
            continue;
        }
        if (ch == '\\') {
            escaping = true;
            continue;
        }
        if (ch == '"') {
            return result;
        }
        result += ch;
    }
    return "";
}

int KasaDiscovery::parseJsonInt(const String& payload, const char* key, int fallback) const {
    const String quotedKey = "\"" + String(key) + "\"";
    int keyIndex = payload.indexOf(quotedKey);
    if (keyIndex < 0) {
        return fallback;
    }

    int colonIndex = payload.indexOf(':', keyIndex + quotedKey.length());
    if (colonIndex < 0) {
        return fallback;
    }

    int valueStart = colonIndex + 1;
    while (valueStart < payload.length() && payload[valueStart] == ' ') {
        ++valueStart;
    }

    int valueEnd = valueStart;
    while (valueEnd < payload.length() && isDigit(payload[valueEnd])) {
        ++valueEnd;
    }

    if (valueEnd == valueStart) {
        return fallback;
    }

    return payload.substring(valueStart, valueEnd).toInt();
}
