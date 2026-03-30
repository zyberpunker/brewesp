#include "network/ProvisioningManager.h"

#include <WiFi.h>
#include <esp_system.h>

namespace {
String htmlEscape(String value) {
    value.replace("&", "&amp;");
    value.replace("\"", "&quot;");
    value.replace("<", "&lt;");
    value.replace(">", "&gt;");
    return value;
}

String driverName(OutputDriverType type) {
    switch (type) {
        case OutputDriverType::Gpio:
            return "gpio";
        case OutputDriverType::ShellyHttpRpc:
            return "shelly_http_rpc";
        case OutputDriverType::KasaLocal:
            return "kasa_local";
        case OutputDriverType::GenericMqttRelay:
            return "generic_mqtt_relay";
        case OutputDriverType::None:
        default:
            return "none";
    }
}

OutputDriverType driverFromName(const String& name) {
    if (name == "gpio") {
        return OutputDriverType::Gpio;
    }
    if (name == "shelly_http_rpc") {
        return OutputDriverType::ShellyHttpRpc;
    }
    if (name == "kasa_local") {
        return OutputDriverType::KasaLocal;
    }
    if (name == "generic_mqtt_relay") {
        return OutputDriverType::GenericMqttRelay;
    }
    return OutputDriverType::None;
}

IPAddress parseIpOrDefault(const String& value) {
    IPAddress address;
    if (address.fromString(value)) {
        return address;
    }
    return IPAddress(192, 168, 4, 1);
}
}

bool ProvisioningManager::begin(const SystemConfig& currentConfig, SaveCallback onSave) {
    currentConfig_ = currentConfig;
    onSave_ = std::move(onSave);
    restartRequested_ = false;

    const IPAddress apIp = parseIpOrDefault(currentConfig_.wifi.recoveryAp.ip);
    const IPAddress gateway = apIp;
    const IPAddress subnet(255, 255, 255, 0);

    accessPointSsid_ = buildRecoverySsid(currentConfig_);
    const String apPassword = currentConfig_.wifi.recoveryAp.password;

    WiFi.disconnect(true);
    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(apIp, gateway, subnet);

    const bool apStarted = WiFi.softAP(
        accessPointSsid_.c_str(),
        apPassword.length() >= 8 ? apPassword.c_str() : nullptr);

    if (!apStarted) {
        Serial.println("[prov] failed to start recovery AP");
        active_ = false;
        return false;
    }

    server_.on("/", HTTP_GET, [this]() { handleRoot(); });
    server_.on("/save", HTTP_POST, [this]() { handleSave(); });
    server_.begin();

    active_ = true;
    Serial.printf(
        "[prov] recovery AP active ssid=%s ip=%s\r\n",
        accessPointSsid_.c_str(),
        WiFi.softAPIP().toString().c_str());
    return true;
}

void ProvisioningManager::update() {
    if (!active_) {
        return;
    }

    server_.handleClient();
}

bool ProvisioningManager::isActive() const {
    return active_;
}

bool ProvisioningManager::restartRequested() const {
    return restartRequested_;
}

const String& ProvisioningManager::accessPointSsid() const {
    return accessPointSsid_;
}

void ProvisioningManager::handleRoot() {
    server_.send(200, "text/html", buildHtmlPage());
}

void ProvisioningManager::handleSave() {
    SystemConfig updated = currentConfig_;

    updated.deviceId = server_.arg("device_id");
    updated.wifi.ssid = server_.arg("wifi_ssid");
    updated.wifi.password = server_.arg("wifi_password");

    updated.mqtt.host = server_.arg("mqtt_host");
    updated.mqtt.port = static_cast<uint16_t>(server_.arg("mqtt_port").toInt());
    updated.mqtt.clientId = server_.arg("mqtt_client_id");
    updated.mqtt.username = server_.arg("mqtt_username");
    updated.mqtt.password = server_.arg("mqtt_password");
    updated.mqtt.topicPrefix = server_.arg("mqtt_topic_prefix");

    updated.heatingOutput.driver = driverFromName(server_.arg("heating_driver"));
    updated.heatingOutput.host = server_.arg("heating_host");
    updated.heatingOutput.port = static_cast<uint16_t>(server_.arg("heating_port").toInt());
    updated.heatingOutput.pin = static_cast<int8_t>(server_.arg("heating_pin").toInt());
    updated.heatingOutput.alias = server_.arg("heating_alias");

    updated.coolingOutput.driver = driverFromName(server_.arg("cooling_driver"));
    updated.coolingOutput.host = server_.arg("cooling_host");
    updated.coolingOutput.port = static_cast<uint16_t>(server_.arg("cooling_port").toInt());
    updated.coolingOutput.pin = static_cast<int8_t>(server_.arg("cooling_pin").toInt());
    updated.coolingOutput.alias = server_.arg("cooling_alias");

    updated.localUi.enabled = server_.hasArg("local_ui_enabled");
    updated.display.enabled = server_.hasArg("display_enabled");
    updated.buttons.enabled = server_.hasArg("buttons_enabled");
    updated.ota.enabled = server_.hasArg("ota_enabled");
    updated.ota.channel = server_.arg("ota_channel");
    updated.ota.checkStrategy = server_.arg("ota_check_strategy");
    updated.ota.checkIntervalSeconds = static_cast<uint32_t>(server_.arg("ota_check_interval_s").toInt());
    updated.ota.manifestUrl = server_.arg("ota_manifest_url");
    updated.ota.caCertFingerprint = server_.arg("ota_ca_cert_fingerprint");
    updated.ota.allowHttp = server_.hasArg("ota_allow_http");

    if (updated.mqtt.port == 0) {
        updated.mqtt.port = 1883;
    }
    if (updated.heatingOutput.port == 0) {
        updated.heatingOutput.port =
            updated.heatingOutput.driver == OutputDriverType::KasaLocal ? 9999 : 80;
    }
    if (updated.coolingOutput.port == 0) {
        updated.coolingOutput.port =
            updated.coolingOutput.driver == OutputDriverType::KasaLocal ? 9999 : 80;
    }
    if (updated.ota.channel.isEmpty()) {
        updated.ota.channel = "stable";
    }
    if (updated.ota.checkStrategy.isEmpty()) {
        updated.ota.checkStrategy = "manual";
    }
    if (updated.ota.checkIntervalSeconds == 0) {
        updated.ota.checkIntervalSeconds = 86400;
    }

    const bool saved = onSave_ && onSave_(updated);
    if (!saved) {
        server_.send(500, "text/plain", "Failed to save configuration");
        return;
    }

    restartRequested_ = true;
    server_.send(
        200,
        "text/html",
        "<html><body><h1>Configuration saved</h1><p>Device will restart.</p></body></html>");
}

String ProvisioningManager::buildHtmlPage() const {
    String page;
    page.reserve(6144);
    page += "<!doctype html><html><head><meta charset='utf-8'><title>brewesp setup</title>";
    page += "<style>body{font-family:sans-serif;max-width:820px;margin:2rem auto;padding:0 1rem;}label{display:block;margin-top:1rem;}input,select{width:100%;padding:.45rem;}fieldset{margin-top:1rem;}button{margin-top:1rem;padding:.7rem 1rem;}</style>";
    page += "</head><body><h1>brewesp setup</h1>";
    page += "<p>Recovery AP: <strong>" + htmlEscape(accessPointSsid_) + "</strong> at <strong>"
            + WiFi.softAPIP().toString() + "</strong></p>";
    page += "<form method='post' action='/save'>";

    page += "<label>Device ID<input name='device_id' value='" + htmlEscape(currentConfig_.deviceId) + "'></label>";
    page += "<label>Wi-Fi SSID<input name='wifi_ssid' value='" + htmlEscape(currentConfig_.wifi.ssid) + "'></label>";
    page += "<label>Wi-Fi password<input name='wifi_password' type='password' value='" + htmlEscape(currentConfig_.wifi.password) + "'></label>";

    page += "<fieldset><legend>MQTT</legend>";
    page += "<label>Host<input name='mqtt_host' value='" + htmlEscape(currentConfig_.mqtt.host) + "'></label>";
    page += "<label>Port<input name='mqtt_port' type='number' value='" + String(currentConfig_.mqtt.port) + "'></label>";
    page += "<label>Client ID<input name='mqtt_client_id' value='" + htmlEscape(currentConfig_.mqtt.clientId) + "'></label>";
    page += "<label>Username<input name='mqtt_username' value='" + htmlEscape(currentConfig_.mqtt.username) + "'></label>";
    page += "<label>Password<input name='mqtt_password' type='password' value='" + htmlEscape(currentConfig_.mqtt.password) + "'></label>";
    page += "<label>Topic prefix<input name='mqtt_topic_prefix' value='" + htmlEscape(currentConfig_.mqtt.topicPrefix) + "'></label>";
    page += "</fieldset>";

    page += "<fieldset><legend>Heating output</legend>";
    page += "<label>Driver<select name='heating_driver'>";
    for (const char* option : {"kasa_local", "gpio", "shelly_http_rpc"}) {
        page += "<option value='" + String(option) + "'";
        if (driverName(currentConfig_.heatingOutput.driver) == option) {
            page += " selected";
        }
        page += ">" + String(option) + "</option>";
    }
    page += "</select></label>";
    page += "<label>Host<input name='heating_host' value='" + htmlEscape(currentConfig_.heatingOutput.host) + "'></label>";
    page += "<label>Port<input name='heating_port' type='number' value='" + String(currentConfig_.heatingOutput.port) + "'></label>";
    page += "<label>GPIO pin<input name='heating_pin' type='number' value='" + String(currentConfig_.heatingOutput.pin) + "'></label>";
    page += "<label>Alias<input name='heating_alias' value='" + htmlEscape(currentConfig_.heatingOutput.alias) + "'></label>";
    page += "</fieldset>";

    page += "<fieldset><legend>Cooling output</legend>";
    page += "<label>Driver<select name='cooling_driver'>";
    for (const char* option : {"kasa_local", "gpio", "shelly_http_rpc"}) {
        page += "<option value='" + String(option) + "'";
        if (driverName(currentConfig_.coolingOutput.driver) == option) {
            page += " selected";
        }
        page += ">" + String(option) + "</option>";
    }
    page += "</select></label>";
    page += "<label>Host<input name='cooling_host' value='" + htmlEscape(currentConfig_.coolingOutput.host) + "'></label>";
    page += "<label>Port<input name='cooling_port' type='number' value='" + String(currentConfig_.coolingOutput.port) + "'></label>";
    page += "<label>GPIO pin<input name='cooling_pin' type='number' value='" + String(currentConfig_.coolingOutput.pin) + "'></label>";
    page += "<label>Alias<input name='cooling_alias' value='" + htmlEscape(currentConfig_.coolingOutput.alias) + "'></label>";
    page += "</fieldset>";

    page += "<fieldset><legend>Local hardware</legend>";
    page += "<label><input type='checkbox' name='local_ui_enabled' ";
    if (currentConfig_.localUi.enabled) {
        page += "checked";
    }
    page += "> Enable local UI</label>";
    page += "<label><input type='checkbox' name='display_enabled' ";
    if (currentConfig_.display.enabled) {
        page += "checked";
    }
    page += "> Display connected</label>";
    page += "<label><input type='checkbox' name='buttons_enabled' ";
    if (currentConfig_.buttons.enabled) {
        page += "checked";
    }
    page += "> Buttons connected</label>";
    page += "</fieldset>";

    page += "<fieldset><legend>OTA</legend>";
    page += "<label><input type='checkbox' name='ota_enabled' ";
    if (currentConfig_.ota.enabled) {
        page += "checked";
    }
    page += "> Enable OTA updates</label>";
    page += "<label>Manifest URL<input name='ota_manifest_url' value='" + htmlEscape(currentConfig_.ota.manifestUrl) + "'></label>";
    page += "<label>Channel<select name='ota_channel'>";
    for (const char* option : {"stable", "beta"}) {
        page += "<option value='" + String(option) + "'";
        if (currentConfig_.ota.channel == option) {
            page += " selected";
        }
        page += ">" + String(option) + "</option>";
    }
    page += "</select></label>";
    page += "<label>Check strategy<select name='ota_check_strategy'>";
    for (const char* option : {"manual", "scheduled"}) {
        page += "<option value='" + String(option) + "'";
        if (currentConfig_.ota.checkStrategy == option) {
            page += " selected";
        }
        page += ">" + String(option) + "</option>";
    }
    page += "</select></label>";
    page += "<label>Check interval (seconds)<input name='ota_check_interval_s' type='number' value='"
            + String(currentConfig_.ota.checkIntervalSeconds) + "'></label>";
    page += "<label>TLS certificate fingerprint<input name='ota_ca_cert_fingerprint' value='"
            + htmlEscape(currentConfig_.ota.caCertFingerprint) + "'></label>";
    page += "<label><input type='checkbox' name='ota_allow_http' ";
    if (currentConfig_.ota.allowHttp) {
        page += "checked";
    }
    page += "> Allow plain HTTP OTA</label>";
    page += "</fieldset>";

    page += "<button type='submit'>Save and reboot</button></form></body></html>";
    return page;
}

String ProvisioningManager::buildRecoverySsid(const SystemConfig& config) const {
    if (!config.wifi.recoveryAp.ssid.isEmpty()) {
        return config.wifi.recoveryAp.ssid;
    }

    const uint64_t mac = ESP.getEfuseMac();
    const uint16_t suffix = static_cast<uint16_t>(mac & 0xFFFF);
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "brewesp-setup-%04X", suffix);
    return String(buffer);
}
