#include "output/ShellyLocalClient.h"

#include <ArduinoJson.h>
#include <WiFi.h>
#include <WiFiClient.h>

namespace {
constexpr uint32_t kConnectTimeoutMs = 60;
constexpr uint32_t kReadTimeoutMs = 250;

bool readHttpResponse(WiFiClient& client, String& body, String* errorReason) {
    const uint32_t startedAt = millis();
    while (!client.available() && client.connected() && millis() - startedAt < kReadTimeoutMs) {
        delay(1);
    }

    const String response = client.readString();
    const int lineEnd = response.indexOf("\r\n");
    if (lineEnd < 0) {
        if (errorReason != nullptr) {
            *errorReason = "invalid http response";
        }
        return false;
    }

    const String statusLine = response.substring(0, lineEnd);
    if (statusLine.indexOf(" 200 ") < 0) {
        if (errorReason != nullptr) {
            *errorReason = "http status " + statusLine;
        }
        return false;
    }

    const int headerEnd = response.indexOf("\r\n\r\n");
    if (headerEnd < 0) {
        if (errorReason != nullptr) {
            *errorReason = "missing http headers";
        }
        return false;
    }

    body = response.substring(headerEnd + 4);
    if (body.isEmpty()) {
        if (errorReason != nullptr) {
            *errorReason = "empty http body";
        }
        return false;
    }

    return true;
}
}

bool ShellyLocalClient::getDeviceInfo(
    const String& host,
    uint16_t port,
    ShellyDeviceInfo& info,
    String* errorReason) {
    String body;
    if (!sendGet(host, port, "/shelly", body, errorReason)) {
        return false;
    }

    DynamicJsonDocument doc(512);
    if (deserializeJson(doc, body) != DeserializationError::Ok) {
        if (errorReason != nullptr) {
            *errorReason = "invalid /shelly json";
        }
        return false;
    }

    info.deviceId = String(static_cast<const char*>(doc["id"] | ""));
    info.model = String(static_cast<const char*>(doc["model"] | ""));
    info.type = String(static_cast<const char*>(doc["type"] | ""));
    info.generation = doc["gen"] | 0;
    info.authEnabled = doc["auth_en"] | false;

    if (info.deviceId.isEmpty() && info.model.isEmpty() && info.type.isEmpty()) {
        if (errorReason != nullptr) {
            *errorReason = "empty /shelly identity";
        }
        return false;
    }

    return true;
}

bool ShellyLocalClient::getRelayStatus(
    const String& host,
    uint16_t port,
    uint8_t switchId,
    ShellyRelayStatus& status,
    String* errorReason) {
    String body;
    String rpcError;
    if (sendGet(host, port, "/rpc/Switch.GetStatus?id=" + String(switchId), body, &rpcError)
        && parseRpcRelayStatus(body, status)) {
        return true;
    }

    String compatError;
    if (sendGet(host, port, "/relay/" + String(switchId), body, &compatError)
        && parseCompatRelayStatus(body, status)) {
        return true;
    }

    if (errorReason != nullptr) {
        *errorReason =
            "relay status unavailable (rpc: " + rpcError + ", compat: " + compatError + ")";
    }
    return false;
}

bool ShellyLocalClient::setRelayState(const String& host, uint16_t port, uint8_t switchId, bool on) {
    String body;
    if (sendGet(
            host,
            port,
            "/rpc/Switch.Set?id=" + String(switchId) + "&on=" + (on ? "true" : "false"),
            body)) {
        return true;
    }

    return sendGet(host, port, "/relay/" + String(switchId) + "?turn=" + (on ? "on" : "off"), body);
}

bool ShellyLocalClient::sendGet(
    const String& host,
    uint16_t port,
    const String& path,
    String& body,
    String* errorReason) {
    if (host.isEmpty() || WiFi.status() != WL_CONNECTED) {
        if (errorReason != nullptr) {
            *errorReason = host.isEmpty() ? "empty host" : "wifi disconnected";
        }
        return false;
    }

    WiFiClient client;
    client.setTimeout(kReadTimeoutMs);
    if (!client.connect(host.c_str(), port, kConnectTimeoutMs)) {
        if (errorReason != nullptr) {
            *errorReason = "connect failed";
        }
        return false;
    }

    client.print("GET ");
    client.print(path);
    client.print(" HTTP/1.1\r\nHost: ");
    client.print(host);
    client.print("\r\nConnection: close\r\n\r\n");

    const bool ok = readHttpResponse(client, body, errorReason);
    client.stop();
    return ok;
}

bool ShellyLocalClient::parseRpcRelayStatus(const String& body, ShellyRelayStatus& status) {
    DynamicJsonDocument doc(512);
    if (deserializeJson(doc, body) != DeserializationError::Ok) {
        return false;
    }

    if (doc["output"].isNull()) {
        return false;
    }

    status.isOn = doc["output"] | false;
    return true;
}

bool ShellyLocalClient::parseCompatRelayStatus(const String& body, ShellyRelayStatus& status) {
    DynamicJsonDocument doc(512);
    if (deserializeJson(doc, body) != DeserializationError::Ok) {
        return false;
    }

    if (doc["ison"].isNull()) {
        return false;
    }

    status.isOn = doc["ison"] | false;
    status.name = String(static_cast<const char*>(doc["name"] | ""));
    return true;
}
