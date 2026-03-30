#include "ota/OtaManager.h"

#include <HTTPClient.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <mbedtls/sha256.h>

#include <ArduinoJson.h>
#include <memory>

#include "config/FirmwareVersion.h"

namespace {
constexpr uint16_t kDefaultHttpsPort = 443;
constexpr uint16_t kDefaultHttpPort = 80;
constexpr size_t kDownloadBufferSize = 1024;
constexpr uint32_t kFermentationSchemaVersion = 1;

String digestToHex(const unsigned char* digest, size_t length) {
    static const char* kHex = "0123456789abcdef";
    String value;
    value.reserve(length * 2);
    for (size_t i = 0; i < length; ++i) {
        value += kHex[(digest[i] >> 4) & 0x0F];
        value += kHex[digest[i] & 0x0F];
    }
    return value;
}

bool isDigitChar(char c) {
    return c >= '0' && c <= '9';
}
}

void OtaManager::begin(const SystemConfig& config) {
    lastCheckStartedMs_ = 0;
    setStatus(config.ota.enabled ? "idle" : "disabled");
}

bool OtaManager::shouldRunScheduledCheck(const SystemConfig& config, uint32_t nowMs) const {
    if (!config.ota.enabled || config.ota.checkStrategy != "scheduled" || config.ota.checkIntervalSeconds == 0) {
        return false;
    }

    if (lastCheckStartedMs_ == 0) {
        return true;
    }

    return nowMs - lastCheckStartedMs_ >= config.ota.checkIntervalSeconds * 1000UL;
}

OtaManager::CheckResult OtaManager::checkForUpdate(const SystemConfig& config, const String& requestedChannel) {
    CheckResult result;
    lastCheckStartedMs_ = millis();

    if (!config.ota.enabled) {
        setStatus("disabled", "OTA disabled");
        result.message = "OTA disabled";
        return result;
    }

    if (WiFi.status() != WL_CONNECTED) {
        setStatus("error", "Wi-Fi not connected");
        result.message = "Wi-Fi not connected";
        return result;
    }

    setStatus("checking");

    Manifest manifest;
    String error;
    if (!fetchManifest(config, requestedChannel, manifest, error)) {
        setStatus("error", error);
        result.message = error;
        return result;
    }

    result.success = true;
    result.manifest = manifest;

    if (compareVersions(manifest.version, FirmwareVersion::kCurrent) <= 0) {
        setStatus("idle", "Already on latest version");
        result.message = "Already on latest version";
        return result;
    }

    setStatus("update_available", "Update available", manifest.version);
    result.updateAvailable = true;
    result.message = "Update available";
    return result;
}

OtaManager::InstallResult OtaManager::startUpdate(const SystemConfig& config, const String& requestedChannel) {
    InstallResult result;
    CheckResult check = checkForUpdate(config, requestedChannel);
    result.manifest = check.manifest;

    if (!check.success) {
        result.message = check.message;
        return result;
    }

    if (!check.updateAvailable) {
        result.success = true;
        result.message = check.message;
        return result;
    }

    setStatus("downloading", "Downloading firmware", check.manifest.version);

    String error;
    if (!downloadAndInstall(config, check.manifest, error)) {
        setStatus("error", error, check.manifest.version);
        result.message = error;
        return result;
    }

    setStatus("rebooting", "Update installed, rebooting", check.manifest.version);
    result.success = true;
    result.message = "Update installed, rebooting";
    return result;
}

const String& OtaManager::status() const {
    return status_;
}

const String& OtaManager::message() const {
    return message_;
}

const String& OtaManager::targetVersion() const {
    return targetVersion_;
}

bool OtaManager::fetchManifest(
    const SystemConfig& config,
    const String& requestedChannel,
    Manifest& manifest,
    String& error) {
    if (config.ota.manifestUrl.isEmpty()) {
        error = "OTA manifest_url is not configured";
        return false;
    }

    String body;
    if (!httpGet(config.ota.manifestUrl, config.ota, body, error)) {
        return false;
    }

    if (!parseManifest(body, manifest, error)) {
        return false;
    }

    const String channel = effectiveChannel(config, requestedChannel);
    if (manifest.channel != channel) {
        error = "Manifest channel does not match requested OTA channel";
        return false;
    }

    if (manifest.minSchemaVersion > kFermentationSchemaVersion) {
        error = "Manifest requires a newer schema version";
        return false;
    }

    return true;
}

bool OtaManager::downloadAndInstall(const SystemConfig& config, const Manifest& manifest, String& error) {
    HTTPClient http;
    std::unique_ptr<WiFiClient> client;
    ParsedUrl parsedUrl;
    if (!beginHttpRequest(manifest.downloadUrl, config.ota, http, client, parsedUrl, error)) {
        return false;
    }

    const int contentLength = http.getSize();
    if (contentLength <= 0) {
        http.end();
        error = "Firmware download is missing a valid content length";
        return false;
    }

    if (!Update.begin(static_cast<size_t>(contentLength), U_FLASH)) {
        http.end();
        error = "Update.begin failed: " + String(Update.errorString());
        return false;
    }

    mbedtls_sha256_context shaContext;
    mbedtls_sha256_init(&shaContext);
    mbedtls_sha256_starts_ret(&shaContext, 0);

    WiFiClient* stream = http.getStreamPtr();
    uint8_t buffer[kDownloadBufferSize];
    size_t remaining = static_cast<size_t>(contentLength);

    while (http.connected() && remaining > 0) {
        const size_t available = stream->available();
        if (available == 0) {
            delay(1);
            continue;
        }

        const size_t toRead = available > sizeof(buffer) ? sizeof(buffer) : available;
        const size_t readCount = stream->readBytes(buffer, toRead);
        if (readCount == 0) {
            Update.abort();
            http.end();
            mbedtls_sha256_free(&shaContext);
            error = "Firmware stream ended unexpectedly";
            return false;
        }

        if (Update.write(buffer, readCount) != readCount) {
            Update.abort();
            http.end();
            mbedtls_sha256_free(&shaContext);
            error = "Update.write failed: " + String(Update.errorString());
            return false;
        }

        mbedtls_sha256_update_ret(&shaContext, buffer, readCount);
        remaining -= readCount;
    }

    unsigned char digest[32];
    mbedtls_sha256_finish_ret(&shaContext, digest);
    mbedtls_sha256_free(&shaContext);

    if (remaining != 0) {
        Update.abort();
        http.end();
        error = "Firmware download did not complete";
        return false;
    }

    const String expectedSha = normalizeCompactHex(manifest.sha256);
    const String actualSha = digestToHex(digest, sizeof(digest));
    if (!expectedSha.isEmpty() && actualSha != expectedSha) {
        Update.abort();
        http.end();
        error = "Firmware SHA256 mismatch";
        return false;
    }

    if (!Update.end()) {
        http.end();
        error = "Update.end failed: " + String(Update.errorString());
        return false;
    }

    http.end();
    return true;
}

bool OtaManager::httpGet(const String& url, const OtaConfig& config, String& body, String& error) {
    HTTPClient http;
    std::unique_ptr<WiFiClient> client;
    ParsedUrl parsedUrl;
    if (!beginHttpRequest(url, config, http, client, parsedUrl, error)) {
        return false;
    }

    body = http.getString();
    http.end();
    return true;
}

bool OtaManager::beginHttpRequest(
    const String& url,
    const OtaConfig& config,
    HTTPClient& http,
    std::unique_ptr<WiFiClient>& client,
    ParsedUrl& parsedUrl,
    String& error) {
    if (!parseUrl(url, parsedUrl, error)) {
        return false;
    }

    if (parsedUrl.scheme == "http" && !config.allowHttp) {
        error = "HTTP downloads are disabled for OTA";
        return false;
    }

    if (parsedUrl.scheme == "https") {
        std::unique_ptr<WiFiClientSecure> secureClient(new WiFiClientSecure());
        secureClient->setInsecure();
        secureClient->setTimeout(15000);
        client = std::move(secureClient);
    } else {
        std::unique_ptr<WiFiClient> plainClient(new WiFiClient());
        plainClient->setTimeout(15000);
        client = std::move(plainClient);
    }

    if (!http.begin(*client, url)) {
        error = "Failed to open OTA URL";
        return false;
    }

    http.setTimeout(15000);
    const int statusCode = http.GET();
    if (statusCode != HTTP_CODE_OK) {
        error = "Unexpected HTTP status " + String(statusCode) + " for OTA request";
        http.end();
        return false;
    }

    if (parsedUrl.scheme == "https") {
        if (config.caCertFingerprint.isEmpty()) {
            error = "HTTPS OTA requires ca_cert_fingerprint";
            http.end();
            return false;
        }

        WiFiClientSecure& secureClient = static_cast<WiFiClientSecure&>(*client);
        if (!secureClient.verify(config.caCertFingerprint.c_str(), parsedUrl.host.c_str())) {
            error = "TLS fingerprint verification failed";
            http.end();
            return false;
        }
    }

    return true;
}

bool OtaManager::parseManifest(const String& body, Manifest& manifest, String& error) const {
    StaticJsonDocument<768> doc;
    const DeserializationError jsonError = deserializeJson(doc, body);
    if (jsonError) {
        error = "Invalid OTA manifest JSON";
        return false;
    }

    manifest.version = String(static_cast<const char*>(doc["version"] | ""));
    manifest.channel = String(static_cast<const char*>(doc["channel"] | ""));
    manifest.publishedAt = String(static_cast<const char*>(doc["published_at"] | ""));
    manifest.minSchemaVersion = doc["min_schema_version"] | 0;
    manifest.sha256 = String(static_cast<const char*>(doc["sha256"] | ""));
    manifest.downloadUrl = String(static_cast<const char*>(doc["download_url"] | ""));

    if (manifest.version.isEmpty() || manifest.channel.isEmpty() || manifest.downloadUrl.isEmpty()) {
        error = "OTA manifest is missing required fields";
        return false;
    }

    manifest.valid = true;
    return true;
}

bool OtaManager::parseUrl(const String& url, ParsedUrl& parsedUrl, String& error) const {
    const int schemeSeparator = url.indexOf("://");
    if (schemeSeparator <= 0) {
        error = "Invalid OTA URL";
        return false;
    }

    parsedUrl.scheme = url.substring(0, schemeSeparator);
    const int hostStart = schemeSeparator + 3;
    const int pathStart = url.indexOf('/', hostStart);
    const String authority = pathStart >= 0 ? url.substring(hostStart, pathStart) : url.substring(hostStart);
    parsedUrl.path = pathStart >= 0 ? url.substring(pathStart) : "/";

    const int portSeparator = authority.indexOf(':');
    if (portSeparator >= 0) {
        parsedUrl.host = authority.substring(0, portSeparator);
        parsedUrl.port = static_cast<uint16_t>(authority.substring(portSeparator + 1).toInt());
    } else {
        parsedUrl.host = authority;
        parsedUrl.port = parsedUrl.scheme == "https" ? kDefaultHttpsPort : kDefaultHttpPort;
    }

    if (parsedUrl.host.isEmpty()) {
        error = "OTA URL is missing a host";
        return false;
    }

    if (parsedUrl.scheme != "http" && parsedUrl.scheme != "https") {
        error = "OTA URL must use http or https";
        return false;
    }

    return true;
}

String OtaManager::effectiveChannel(const SystemConfig& config, const String& requestedChannel) const {
    return requestedChannel.isEmpty() ? config.ota.channel : requestedChannel;
}

String OtaManager::normalizeCompactHex(const String& value) const {
    String normalized;
    normalized.reserve(value.length());
    for (size_t i = 0; i < value.length(); ++i) {
        const char ch = value[i];
        if (ch == ':' || ch == ' ' || ch == '\t') {
            continue;
        }
        normalized += static_cast<char>(tolower(ch));
    }
    return normalized;
}

int OtaManager::compareVersions(const String& lhs, const String& rhs) const {
    size_t lhsIndex = 0;
    size_t rhsIndex = 0;

    while (lhsIndex < lhs.length() || rhsIndex < rhs.length()) {
        while (lhsIndex < lhs.length() && !isDigitChar(lhs[lhsIndex])) {
            ++lhsIndex;
        }
        while (rhsIndex < rhs.length() && !isDigitChar(rhs[rhsIndex])) {
            ++rhsIndex;
        }

        unsigned long lhsValue = 0;
        unsigned long rhsValue = 0;

        while (lhsIndex < lhs.length() && isDigitChar(lhs[lhsIndex])) {
            lhsValue = lhsValue * 10UL + static_cast<unsigned long>(lhs[lhsIndex] - '0');
            ++lhsIndex;
        }

        while (rhsIndex < rhs.length() && isDigitChar(rhs[rhsIndex])) {
            rhsValue = rhsValue * 10UL + static_cast<unsigned long>(rhs[rhsIndex] - '0');
            ++rhsIndex;
        }

        if (lhsValue != rhsValue) {
            return lhsValue > rhsValue ? 1 : -1;
        }
    }

    return lhs.compareTo(rhs);
}

void OtaManager::setStatus(const String& status, const String& message, const String& targetVersion) {
    status_ = status;
    message_ = message;
    targetVersion_ = targetVersion;
}
