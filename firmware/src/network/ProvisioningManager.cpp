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

void appendTextField(
    String& page,
    const char* label,
    const char* name,
    const String& value,
    const char* type = "text",
    const char* inputMode = nullptr) {
    page += "<label class='field'><span class='field-label'>" + String(label) + "</span><input name='"
            + String(name) + "' type='" + String(type) + "'";
    if (inputMode != nullptr) {
        page += " inputmode='" + String(inputMode) + "'";
    }
    page += " value='" + htmlEscape(value) + "'></label>";
}

void appendPasswordField(String& page, const char* label, const char* name, const String& value) {
    page += "<label class='field'><span class='field-label'>" + String(label) + "</span>";
    page += "<span class='password-field'><input name='" + String(name)
            + "' type='password' value='" + htmlEscape(value) + "'>";
    page += "<button class='password-toggle' type='button' data-password-toggle aria-label='Show password' aria-pressed='false'>";
    page += "<svg viewBox='0 0 24 24' aria-hidden='true' focusable='false'><path d='M12 5C7 5 2.73 8.11 1 12c1.73 3.89 6 7 11 7s9.27-3.11 11-7c-1.73-3.89-6-7-11-7Zm0 11a4 4 0 1 1 0-8 4 4 0 0 1 0 8Zm0-2.5a1.5 1.5 0 1 0 0-3 1.5 1.5 0 0 0 0 3Z'/></svg>";
    page += "</button></span></label>";
}

void appendNumberField(String& page, const char* label, const char* name, long value) {
    appendTextField(page, label, name, String(value), "number", "numeric");
}

void appendCheckboxField(String& page, const char* name, const char* label, bool checked) {
    page += "<label class='toggle'><input type='checkbox' name='" + String(name) + "'";
    if (checked) {
        page += " checked";
    }
    page += "><span>" + String(label) + "</span></label>";
}

void appendSectionHeader(String& page, const char* eyebrow, const char* title, const char* description) {
    page += "<section class='card'><div class='section-heading'><p class='eyebrow'>" + String(eyebrow)
            + "</p><h2>" + String(title) + "</h2><p>" + String(description) + "</p></div>";
}

String buildSavedHtmlPage() {
    String page;
    page.reserve(1792);
    page += "<!doctype html><html><head><meta charset='utf-8'>";
    page += "<meta name='viewport' content='width=device-width,initial-scale=1,viewport-fit=cover'>";
    page += "<title>brewesp setup</title>";
    page += "<style>";
    page +=
        ":root{color-scheme:light;font-family:Inter,Segoe UI,Arial,sans-serif;"
        "--bg:#f4f7fb;--panel:#ffffff;--text:#142033;--muted:#5f6f86;--line:#d8e0ec;"
        "--accent:#0f766e;--accent-strong:#115e59;--shadow:0 18px 48px rgba(15,23,42,.12);}"
        "*{box-sizing:border-box}body{margin:0;min-height:100vh;padding:24px;background:linear-gradient(180deg,#eef7f7 0%,#f7fafc 100%);"
        "color:var(--text);display:flex;align-items:center;justify-content:center}"
        ".panel{width:min(100%,560px);background:var(--panel);border:1px solid rgba(216,224,236,.9);"
        "border-radius:24px;padding:28px;box-shadow:var(--shadow)}"
        ".badge{display:inline-flex;align-items:center;gap:8px;padding:8px 12px;border-radius:999px;"
        "background:#dcfce7;color:#166534;font-size:13px;font-weight:700;letter-spacing:.02em;text-transform:uppercase}"
        "h1{margin:18px 0 12px;font-size:clamp(28px,6vw,38px);line-height:1.05}"
        "p{margin:0 0 18px;color:var(--muted);font-size:15px;line-height:1.6}"
        ".button{display:inline-flex;align-items:center;justify-content:center;min-height:48px;padding:0 18px;"
        "border-radius:14px;background:var(--accent);color:#fff;text-decoration:none;font-weight:700}"
        ".button:active{background:var(--accent-strong)}"
        "</style></head><body><main class='panel'><span class='badge'>Configuration saved</span>";
    page += "<h1>brewesp is restarting</h1>";
    page += "<p>Your settings were stored successfully. Keep this page open for a moment, then reconnect to the device on your normal Wi-Fi network.</p>";
    page += "<a class='button' href='/'>Reload setup page</a>";
    page += "</main></body></html>";
    return page;
}
}

bool ProvisioningManager::begin(const SystemConfig& currentConfig, SaveCallback onSave) {
    currentConfig_ = currentConfig;
    onSave_ = std::move(onSave);
    restartRequested_ = false;
    active_ = false;

    const IPAddress apIp = parseIpOrDefault(currentConfig_.wifi.recoveryAp.ip);
    const IPAddress gateway = apIp;
    const IPAddress subnet(255, 255, 255, 0);
    const bool keepStationActive = !currentConfig_.wifi.ssid.isEmpty();

    accessPointSsid_ = buildRecoverySsid(currentConfig_);
    const String apPassword = currentConfig_.wifi.recoveryAp.password;

    if (!keepStationActive) {
        WiFi.disconnect(true);
    }
    WiFi.mode(keepStationActive ? WIFI_AP_STA : WIFI_AP);
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
    server_.send(200, "text/html", buildSavedHtmlPage());
}

String ProvisioningManager::buildHtmlPage() const {
    String page;
    page.reserve(16384);
    page += "<!doctype html><html><head><meta charset='utf-8'>";
    page += "<meta name='viewport' content='width=device-width,initial-scale=1,viewport-fit=cover'>";
    page += "<title>brewesp setup</title>";
    page += "<style>";
    page +=
        ":root{color-scheme:light;font-family:Inter,Segoe UI,Arial,sans-serif;"
        "--bg:#f3f7fb;--panel:#ffffff;--panel-alt:#f8fbff;--text:#122033;--muted:#5f6f86;"
        "--line:#d7dfeb;--line-strong:#bfd0e3;--accent:#0f766e;--accent-strong:#115e59;"
        "--accent-soft:#dff5f1;--chip:#edf4ff;--shadow:0 22px 60px rgba(15,23,42,.12);}"
        "*{box-sizing:border-box}html{font-size:16px}body{margin:0;background:radial-gradient(circle at top,#e6f5f2 0,#f4f7fb 42%,#eef3f9 100%);"
        "color:var(--text)}main{width:min(100%,1120px);margin:0 auto;padding:20px 14px 32px}"
        ".hero{position:relative;overflow:hidden;background:linear-gradient(145deg,#0f172a 0%,#15325d 55%,#0f766e 100%);"
        "color:#fff;border-radius:28px;padding:24px;box-shadow:var(--shadow)}"
        ".hero:after{content:'';position:absolute;inset:auto -60px -80px auto;width:220px;height:220px;border-radius:50%;"
        "background:radial-gradient(circle,rgba(255,255,255,.22) 0,rgba(255,255,255,0) 72%)}"
        ".eyebrow{margin:0 0 8px;font-size:12px;font-weight:800;letter-spacing:.14em;text-transform:uppercase;opacity:.78}"
        "h1{margin:0;font-size:clamp(2rem,8vw,3.4rem);line-height:1.02;letter-spacing:-.04em;max-width:10ch}"
        ".hero-copy{margin:14px 0 0;color:rgba(255,255,255,.82);font-size:15px;line-height:1.6;max-width:44rem}"
        ".status-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:12px;margin-top:18px}"
        ".status-card{backdrop-filter:blur(14px);background:rgba(255,255,255,.12);border:1px solid rgba(255,255,255,.18);"
        "border-radius:18px;padding:14px 16px}.status-label{display:block;font-size:11px;font-weight:800;letter-spacing:.12em;text-transform:uppercase;opacity:.72}"
        ".status-value{display:block;margin-top:6px;font-size:16px;font-weight:700;line-height:1.35;word-break:break-word}"
        "form{display:grid;gap:16px;margin-top:16px}.card{background:rgba(255,255,255,.92);border:1px solid rgba(215,223,235,.92);"
        "border-radius:24px;padding:18px;box-shadow:0 12px 32px rgba(15,23,42,.05)}"
        ".section-heading{margin-bottom:14px}.section-heading h2{margin:0;font-size:1.25rem;letter-spacing:-.02em}"
        ".section-heading p{margin:8px 0 0;color:var(--muted);font-size:14px;line-height:1.55}"
        ".grid{display:grid;grid-template-columns:1fr;gap:12px}.field,.toggle{display:block}.field-label{display:block;margin-bottom:6px;"
        "font-size:13px;font-weight:700;color:#334155}.field input,.field select{width:100%;min-height:48px;border-radius:14px;"
        "border:1px solid var(--line);background:var(--panel-alt);padding:12px 14px;font:inherit;color:var(--text);appearance:none}"
        ".field input:focus,.field select:focus{outline:none;border-color:var(--accent);box-shadow:0 0 0 4px rgba(15,118,110,.14)}"
        ".password-field{display:grid;grid-template-columns:minmax(0,1fr) 52px;gap:8px;align-items:center}"
        ".password-field input{margin:0}.password-toggle{display:inline-flex;align-items:center;justify-content:center;min-height:48px;"
        "border:1px solid var(--line);border-radius:14px;background:var(--panel-alt);color:#334155;padding:0;cursor:pointer}"
        ".password-toggle svg{width:20px;height:20px;fill:currentColor}.password-toggle:focus{outline:none;border-color:var(--accent);"
        "box-shadow:0 0 0 4px rgba(15,118,110,.14)}.password-toggle[aria-pressed='true']{background:var(--accent-soft);color:var(--accent-strong)}"
        ".toggle{display:flex;align-items:center;gap:12px;min-height:52px;border:1px solid var(--line);border-radius:16px;"
        "background:var(--panel-alt);padding:12px 14px;font-weight:600;color:#243247}"
        ".toggle input{width:20px;height:20px;margin:0;accent-color:var(--accent);flex:0 0 auto}"
        ".actions{position:sticky;bottom:12px;background:rgba(244,247,251,.84);backdrop-filter:blur(12px);border:1px solid rgba(215,223,235,.9);"
        "border-radius:22px;padding:12px;box-shadow:0 10px 30px rgba(15,23,42,.08)}"
        ".actions p{margin:0 0 10px;color:var(--muted);font-size:13px;line-height:1.45}.button{width:100%;min-height:52px;border:0;"
        "border-radius:16px;padding:0 18px;background:linear-gradient(135deg,var(--accent) 0%,#0b8c80 100%);color:#fff;font:inherit;font-weight:800;"
        "letter-spacing:.01em;box-shadow:0 14px 28px rgba(15,118,110,.24)}"
        ".button:active{transform:translateY(1px)}@media (min-width:720px){main{padding:24px 20px 40px}.hero{padding:30px}"
        ".card{padding:24px}.grid{grid-template-columns:repeat(2,minmax(0,1fr))}.grid .full{grid-column:1 / -1}.actions{padding:14px 16px}}";
    page += "</style></head><body><main>";
    page += "<section class='hero'><p class='eyebrow'>First-time setup</p><h1>brewesp firmware v2</h1>";
    page += "<p class='hero-copy'>Connect the controller to Wi-Fi, point it at your MQTT broker, and choose how heating and cooling are wired. This page is hosted directly on the ESP recovery access point.</p>";
    page += "<div class='status-grid'>";
    page += "<div class='status-card'><span class='status-label'>Recovery AP</span><span class='status-value'>"
            + htmlEscape(accessPointSsid_) + "</span></div>";
    page += "<div class='status-card'><span class='status-label'>Setup IP</span><span class='status-value'>"
            + WiFi.softAPIP().toString() + "</span></div>";
    page += "<div class='status-card'><span class='status-label'>Current device ID</span><span class='status-value'>"
            + htmlEscape(currentConfig_.deviceId) + "</span></div>";
    page += "</div></section>";
    page += "<form method='post' action='/save'>";

    appendSectionHeader(
        page,
        "Basics",
        "Device and Wi-Fi",
        "These settings get the ESP onto your normal network so the rest of the stack can find it.");
    page += "<div class='grid'>";
    appendTextField(page, "Device ID", "device_id", currentConfig_.deviceId);
    appendTextField(page, "Wi-Fi SSID", "wifi_ssid", currentConfig_.wifi.ssid);
    appendPasswordField(page, "Wi-Fi password", "wifi_password", currentConfig_.wifi.password);
    page += "</div></section>";

    appendSectionHeader(
        page,
        "Messaging",
        "MQTT",
        "Broker details used for telemetry, state, and remote configuration.");
    page += "<div class='grid'>";
    appendTextField(page, "Host", "mqtt_host", currentConfig_.mqtt.host);
    appendNumberField(page, "Port", "mqtt_port", currentConfig_.mqtt.port);
    appendTextField(page, "Client ID", "mqtt_client_id", currentConfig_.mqtt.clientId);
    appendTextField(page, "Username", "mqtt_username", currentConfig_.mqtt.username);
    appendPasswordField(page, "Password", "mqtt_password", currentConfig_.mqtt.password);
    appendTextField(page, "Topic prefix", "mqtt_topic_prefix", currentConfig_.mqtt.topicPrefix);
    page += "</div></section>";

    appendSectionHeader(
        page,
        "Outputs",
        "Heating output",
        "Select the driver and target used when the controller asks for heat.");
    page += "<div class='grid'>";
    page += "<label class='field'><span class='field-label'>Driver</span><select name='heating_driver'>";
    for (const char* option : {"kasa_local", "gpio", "shelly_http_rpc"}) {
        page += "<option value='" + String(option) + "'";
        if (driverName(currentConfig_.heatingOutput.driver) == option) {
            page += " selected";
        }
        page += ">" + String(option) + "</option>";
    }
    page += "</select></label>";
    appendTextField(page, "Host", "heating_host", currentConfig_.heatingOutput.host);
    appendNumberField(page, "Port", "heating_port", currentConfig_.heatingOutput.port);
    appendNumberField(page, "GPIO pin", "heating_pin", currentConfig_.heatingOutput.pin);
    appendTextField(page, "Alias", "heating_alias", currentConfig_.heatingOutput.alias);
    page += "</div></section>";

    appendSectionHeader(
        page,
        "Outputs",
        "Cooling output",
        "Select the driver and target used when the controller asks for cooling.");
    page += "<div class='grid'>";
    page += "<label class='field'><span class='field-label'>Driver</span><select name='cooling_driver'>";
    for (const char* option : {"kasa_local", "gpio", "shelly_http_rpc"}) {
        page += "<option value='" + String(option) + "'";
        if (driverName(currentConfig_.coolingOutput.driver) == option) {
            page += " selected";
        }
        page += ">" + String(option) + "</option>";
    }
    page += "</select></label>";
    appendTextField(page, "Host", "cooling_host", currentConfig_.coolingOutput.host);
    appendNumberField(page, "Port", "cooling_port", currentConfig_.coolingOutput.port);
    appendNumberField(page, "GPIO pin", "cooling_pin", currentConfig_.coolingOutput.pin);
    appendTextField(page, "Alias", "cooling_alias", currentConfig_.coolingOutput.alias);
    page += "</div></section>";

    appendSectionHeader(
        page,
        "Hardware",
        "Local peripherals",
        "Turn these on only for hardware that is actually attached to this controller.");
    page += "<div class='grid'>";
    appendCheckboxField(page, "local_ui_enabled", "Enable local UI", currentConfig_.localUi.enabled);
    appendCheckboxField(page, "display_enabled", "Display connected", currentConfig_.display.enabled);
    appendCheckboxField(page, "buttons_enabled", "Buttons connected", currentConfig_.buttons.enabled);
    page += "</div></section>";

    appendSectionHeader(
        page,
        "Updates",
        "OTA",
        "Firmware update settings stored locally on the ESP.");
    page += "<div class='grid'>";
    appendCheckboxField(page, "ota_enabled", "Enable OTA updates", currentConfig_.ota.enabled);
    appendTextField(page, "Manifest URL", "ota_manifest_url", currentConfig_.ota.manifestUrl);
    page += "<label class='field'><span class='field-label'>Channel</span><select name='ota_channel'>";
    for (const char* option : {"stable", "beta"}) {
        page += "<option value='" + String(option) + "'";
        if (currentConfig_.ota.channel == option) {
            page += " selected";
        }
        page += ">" + String(option) + "</option>";
    }
    page += "</select></label>";
    page += "<label class='field'><span class='field-label'>Check strategy</span><select name='ota_check_strategy'>";
    for (const char* option : {"manual", "scheduled"}) {
        page += "<option value='" + String(option) + "'";
        if (currentConfig_.ota.checkStrategy == option) {
            page += " selected";
        }
        page += ">" + String(option) + "</option>";
    }
    page += "</select></label>";
    appendNumberField(
        page,
        "Check interval (seconds)",
        "ota_check_interval_s",
        currentConfig_.ota.checkIntervalSeconds);
    appendTextField(
        page,
        "TLS certificate fingerprint",
        "ota_ca_cert_fingerprint",
        currentConfig_.ota.caCertFingerprint);
    appendCheckboxField(page, "ota_allow_http", "Allow plain HTTP OTA", currentConfig_.ota.allowHttp);
    page += "</div></section>";

    page += "<section class='actions'><p>Saving writes the local system configuration to NVS and restarts the ESP so the new network settings can take effect.</p>";
    page += "<button class='button' type='submit'>Save and reboot</button></section>";
    page += "</form>";
    page += "<script>(function(){var toggles=document.querySelectorAll('[data-password-toggle]');for(var i=0;i<toggles.length;i++){toggles[i].addEventListener('click',function(){var field=this.parentElement.querySelector('input');if(!field){return;}var showing=field.type==='text';field.type=showing?'password':'text';this.setAttribute('aria-pressed',showing?'false':'true');this.setAttribute('aria-label',showing?'Show password':'Hide password');});}})();</script>";
    page += "</main></body></html>";
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
