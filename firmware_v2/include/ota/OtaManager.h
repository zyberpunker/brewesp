#pragma once

#include <Arduino.h>

#include <memory>

#include "config/SystemConfig.h"

class HTTPClient;
class WiFiClient;

class OtaManager {
public:
    struct Manifest {
        bool valid = false;
        String version;
        String channel;
        String publishedAt;
        uint32_t minSchemaVersion = 0;
        String sha256;
        String downloadUrl;
    };

    struct CheckResult {
        bool success = false;
        bool updateAvailable = false;
        String message;
        Manifest manifest;
    };

    struct InstallResult {
        bool success = false;
        String message;
        Manifest manifest;
    };

    void begin(const SystemConfig& config);
    bool shouldRunScheduledCheck(const SystemConfig& config, uint32_t nowMs) const;
    CheckResult checkForUpdate(const SystemConfig& config, const String& requestedChannel = "");
    InstallResult startUpdate(const SystemConfig& config, const String& requestedChannel = "");
    const String& status() const;
    const String& message() const;
    const String& targetVersion() const;

private:
    struct ParsedUrl {
        String scheme;
        String host;
        uint16_t port = 0;
        String path = "/";
    };

    bool fetchManifest(const SystemConfig& config, const String& requestedChannel, Manifest& manifest, String& error);
    bool downloadAndInstall(const SystemConfig& config, const Manifest& manifest, String& error);
    bool httpGet(const String& url, const OtaConfig& config, String& body, String& error);
    bool beginHttpRequest(
        const String& url,
        const OtaConfig& config,
        HTTPClient& http,
        std::unique_ptr<WiFiClient>& client,
        ParsedUrl& parsedUrl,
        String& error);
    bool parseManifest(const String& body, Manifest& manifest, String& error) const;
    bool parseUrl(const String& url, ParsedUrl& parsedUrl, String& error) const;
    String effectiveChannel(const SystemConfig& config, const String& requestedChannel) const;
    String normalizeCompactHex(const String& value) const;
    int compareVersions(const String& lhs, const String& rhs) const;
    void setStatus(const String& status, const String& message = "", const String& targetVersion = "");

    uint32_t lastCheckStartedMs_ = 0;
    String status_ = "idle";
    String message_;
    String targetVersion_;
};
