#include "output/ShellyDiscovery.h"

#include <WiFi.h>

#include "output/ShellyLocalClient.h"
#include "support/Logger.h"

namespace {
constexpr uint32_t kMinBudgetPerHostMs = 80;
constexpr uint32_t kProgressLogIntervalHosts = 64;

uint32_t ipToUint32(const IPAddress& ip) {
    return (static_cast<uint32_t>(ip[0]) << 24) | (static_cast<uint32_t>(ip[1]) << 16)
        | (static_cast<uint32_t>(ip[2]) << 8) | static_cast<uint32_t>(ip[3]);
}

IPAddress uint32ToIp(uint32_t value) {
    return IPAddress(
        static_cast<uint8_t>((value >> 24) & 0xFF),
        static_cast<uint8_t>((value >> 16) & 0xFF),
        static_cast<uint8_t>((value >> 8) & 0xFF),
        static_cast<uint8_t>(value & 0xFF));
}
}

bool ShellyDiscovery::discover(DeviceCallback callback, uint32_t timeoutMs) {
    if (WiFi.status() != WL_CONNECTED) {
        LOG_DEBUG_MSG("[shelly-discovery] skipped because wifi is disconnected");
        return false;
    }

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
    const uint32_t networkValue = ipToUint32(network);
    const uint32_t broadcastValue = ipToUint32(broadcast);
    const uint32_t hostCount =
        broadcastValue > networkValue ? broadcastValue - networkValue - 1 : 0;
    const uint32_t scanBudgetMs =
        hostCount * kMinBudgetPerHostMs > timeoutMs ? hostCount * kMinBudgetPerHostMs : timeoutMs;

    LOG_DEBUG(
        "[shelly-discovery] start local=%s mask=%s network=%s broadcast=%s hosts=%lu budget_ms=%lu\r\n",
        localIp.toString().c_str(),
        subnetMask.toString().c_str(),
        network.toString().c_str(),
        broadcast.toString().c_str(),
        static_cast<unsigned long>(hostCount),
        static_cast<unsigned long>(scanBudgetMs));

    bool found = false;
    uint32_t foundCount = 0;
    uint32_t probedHosts = 0;
    bool timedOut = false;
    const uint32_t startedAt = millis();
    for (uint32_t candidateValue = networkValue + 1; candidateValue < broadcastValue; ++candidateValue) {
        if (millis() - startedAt >= scanBudgetMs) {
            timedOut = true;
            break;
        }

        IPAddress candidate = uint32ToIp(candidateValue);
        if (candidate == localIp) {
            continue;
        }

        ++probedHosts;
        String failureReason;
        if (probeHost(candidate.toString(), callback, &failureReason)) {
            found = true;
            ++foundCount;
        } else if (!failureReason.isEmpty()) {
            LOG_DEBUG(
                "[shelly-discovery] host=%s rejected reason=%s\r\n",
                candidate.toString().c_str(),
                failureReason.c_str());
        }

        if (probedHosts % kProgressLogIntervalHosts == 0) {
            LOG_DEBUG(
                "[shelly-discovery] progress probed=%lu/%lu found=%lu elapsed_ms=%lu\r\n",
                static_cast<unsigned long>(probedHosts),
                static_cast<unsigned long>(hostCount),
                static_cast<unsigned long>(foundCount),
                static_cast<unsigned long>(millis() - startedAt));
        }
        delay(1);
    }

    LOG_DEBUG(
        "[shelly-discovery] complete probed=%lu/%lu found=%lu elapsed_ms=%lu timed_out=%s\r\n",
        static_cast<unsigned long>(probedHosts),
        static_cast<unsigned long>(hostCount),
        static_cast<unsigned long>(foundCount),
        static_cast<unsigned long>(millis() - startedAt),
        timedOut ? "true" : "false");

    return found;
}

String ShellyDiscovery::escapeJson(const String& value) const {
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

bool ShellyDiscovery::probeHost(const String& host, DeviceCallback callback, String* failureReason) const {
    ShellyDeviceInfo info;
    if (!ShellyLocalClient::getDeviceInfo(host, 80, info)) {
        return false;
    }

    ShellyRelayStatus relayStatus;
    if (!ShellyLocalClient::getRelayStatus(host, 80, 0, relayStatus, failureReason)) {
        return false;
    }

    String payload;
    payload.reserve(320);
    payload += "{\"driver\":\"shelly_http_rpc\"";
    payload += ",\"host\":\"" + escapeJson(host) + "\"";
    payload += ",\"port\":80";
    payload += ",\"switch_id\":0";

    const String alias = !relayStatus.name.isEmpty() ? relayStatus.name : info.deviceId;
    if (!alias.isEmpty()) {
        payload += ",\"alias\":\"" + escapeJson(alias) + "\"";
    }

    const String model = !info.model.isEmpty() ? info.model : info.type;
    if (!model.isEmpty()) {
        payload += ",\"model\":\"" + escapeJson(model) + "\"";
    }

    if (!info.deviceId.isEmpty()) {
        payload += ",\"device_id\":\"" + escapeJson(info.deviceId) + "\"";
    }

    if (info.generation > 0) {
        payload += ",\"generation\":";
        payload += String(info.generation);
    }

    payload += ",\"auth_required\":";
    payload += info.authEnabled ? "true" : "false";
    payload += ",\"is_on\":";
    payload += relayStatus.isOn ? "true" : "false";
    payload += "}";

    callback(payload);
    LOG_DEBUG(
        "[shelly-discovery] found host=%s alias=%s model=%s switch_id=0 state=%s\r\n",
        host.c_str(),
        alias.c_str(),
        model.c_str(),
        relayStatus.isOn ? "on" : "off");
    return true;
}
