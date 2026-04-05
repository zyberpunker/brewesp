#include "provisioning_server.h"

#include "config_store.h"

namespace {
String htmlEscape(String value) {
  value.replace("&", "&amp;");
  value.replace("<", "&lt;");
  value.replace(">", "&gt;");
  value.replace("\"", "&quot;");
  return value;
}
}  // namespace

void ProvisioningServer::begin(const SystemConfig &defaults) {
  defaults_ = defaults;
  last_message_ = "Enter Wi-Fi and output settings. MQTT is required only when config owner is external.";

  server_.on("/", HTTP_GET, [this]() { handleRoot(); });
  server_.on("/save", HTTP_POST, [this]() { handleSave(); });
  server_.on("/api/system-config", HTTP_POST, [this]() { handleApiSave(); });
  server_.onNotFound([this]() { handleNotFound(); });
  server_.begin();
}

void ProvisioningServer::loop() {
  server_.handleClient();
}

bool ProvisioningServer::consumePendingConfig(SystemConfig &config) {
  if (!has_pending_) {
    return false;
  }
  config = pending_;
  has_pending_ = false;
  return true;
}

String ProvisioningServer::formValue(const char *key, const String &fallback) {
  if (server_.hasArg(key)) {
    return server_.arg(key);
  }
  return fallback;
}

int ProvisioningServer::formIntValue(const char *key, int fallback) {
  if (server_.hasArg(key)) {
    return server_.arg(key).toInt();
  }
  return fallback;
}

bool ProvisioningServer::formChecked(const char *key, bool fallback) {
  if (!server_.hasArg(key)) {
    return fallback;
  }
  const String value = server_.arg(key);
  return value == "on" || value == "true" || value == "1";
}

SystemConfig ProvisioningServer::buildFormConfig() {
  SystemConfig config = defaults_;
  config.device_id = formValue("device_id", defaults_.device_id);
  config.timezone = formValue("timezone", defaults_.timezone);
  config.config_owner = formValue("config_owner", defaults_.config_owner);
  config.wifi.ssid = formValue("wifi_ssid", defaults_.wifi.ssid);
  config.wifi.password = formValue("wifi_password", defaults_.wifi.password);
  config.mqtt.host = formValue("mqtt_host", defaults_.mqtt.host);
  config.mqtt.port = static_cast<uint16_t>(formIntValue("mqtt_port", defaults_.mqtt.port));
  config.mqtt.client_id = formValue("mqtt_client_id", defaults_.mqtt.client_id);
  config.mqtt.topic_prefix = formValue("mqtt_topic_prefix", defaults_.mqtt.topic_prefix);
  config.mqtt.username = formValue("mqtt_username", defaults_.mqtt.username);
  config.mqtt.password = formValue("mqtt_password", defaults_.mqtt.password);
  config.heartbeat.interval_s = static_cast<uint32_t>(formIntValue("heartbeat_interval_s", defaults_.heartbeat.interval_s));
  config.ota.manifest_url = formValue("ota_manifest_url", defaults_.ota.manifest_url);
  config.ota.allow_http = formChecked("ota_allow_http", defaults_.ota.allow_http);

  config.heating.driver = formValue("heating_driver", defaults_.heating.driver);
  config.heating.pin = formIntValue("heating_pin", defaults_.heating.pin);
  config.heating.active_level = formValue("heating_active_level", defaults_.heating.active_level);
  config.heating.host = formValue("heating_host", defaults_.heating.host);
  config.heating.port = static_cast<uint16_t>(formIntValue("heating_port", defaults_.heating.port));
  config.heating.switch_id = static_cast<uint8_t>(formIntValue("heating_switch_id", defaults_.heating.switch_id));
  config.heating.username = formValue("heating_username", defaults_.heating.username);
  config.heating.password = formValue("heating_password", defaults_.heating.password);
  config.heating.alias = formValue("heating_alias", defaults_.heating.alias);

  config.cooling.driver = formValue("cooling_driver", defaults_.cooling.driver);
  config.cooling.pin = formIntValue("cooling_pin", defaults_.cooling.pin);
  config.cooling.active_level = formValue("cooling_active_level", defaults_.cooling.active_level);
  config.cooling.host = formValue("cooling_host", defaults_.cooling.host);
  config.cooling.port = static_cast<uint16_t>(formIntValue("cooling_port", defaults_.cooling.port));
  config.cooling.switch_id = static_cast<uint8_t>(formIntValue("cooling_switch_id", defaults_.cooling.switch_id));
  config.cooling.username = formValue("cooling_username", defaults_.cooling.username);
  config.cooling.password = formValue("cooling_password", defaults_.cooling.password);
  config.cooling.alias = formValue("cooling_alias", defaults_.cooling.alias);
  return config;
}

String ProvisioningServer::renderPage() const {
  String page;
  page.reserve(8192);
  page += "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
  page += "<title>BrewESP Recovery</title><style>";
  page += "body{font-family:Segoe UI,Arial,sans-serif;margin:0;background:#f4f1ea;color:#1f2a30;}main{max-width:980px;margin:0 auto;padding:24px;}";
  page += "h1{margin-top:0;}section{background:#fff;border-radius:12px;padding:16px;margin-bottom:16px;box-shadow:0 4px 14px rgba(0,0,0,.08);}label{display:block;font-weight:600;margin:10px 0 6px;}";
  page += "input,select{width:100%;padding:10px;border:1px solid #c9ced3;border-radius:8px;box-sizing:border-box;}button{background:#1f6b75;color:#fff;border:0;padding:12px 18px;border-radius:8px;font-weight:700;cursor:pointer;}small{color:#55636d;} .grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(240px,1fr));gap:12px;}";
  page += ".banner{background:#dfeee8;border-left:4px solid #1f6b75;padding:12px 14px;border-radius:8px;margin-bottom:16px;}</style></head><body><main>";
  page += "<h1>BrewESP v2 recovery AP</h1>";
  page += "<div class='banner'>" + htmlEscape(last_message_) + "</div>";
  page += "<form method='post' action='/save'>";
  page += "<section><h2>Device</h2><div class='grid'>";
  page += "<div><label>Device ID</label><input name='device_id' value='" + htmlEscape(defaults_.device_id) + "'></div>";
  page += "<div><label>Timezone</label><input name='timezone' value='" + htmlEscape(defaults_.timezone) + "'></div>";
  page += "<div><label>Config owner</label><select name='config_owner'><option value='local' " +
          String(defaults_.config_owner == kConfigOwnerLocal ? "selected" : "") +
          ">local</option><option value='external' " +
          String(defaults_.config_owner == kConfigOwnerExternal ? "selected" : "") + ">external</option></select></div>";
  page += "<div><label>Heartbeat interval (s)</label><input name='heartbeat_interval_s' type='number' value='" + String(defaults_.heartbeat.interval_s) + "'></div>";
  page += "<div><label>OTA manifest URL</label><input name='ota_manifest_url' value='" + htmlEscape(defaults_.ota.manifest_url) + "'></div>";
  page += "</div><label><input type='checkbox' name='ota_allow_http' " + String(defaults_.ota.allow_http ? "checked" : "") + "> Allow OTA over plain HTTP</label></section>";
  page += "<section><h2>Wi-Fi</h2><div class='grid'>";
  page += "<div><label>SSID</label><input name='wifi_ssid' value='" + htmlEscape(defaults_.wifi.ssid) + "'></div>";
  page += "<div><label>Password</label><input name='wifi_password' type='password' value='" + htmlEscape(defaults_.wifi.password) + "'></div>";
  page += "</div></section>";
  page += "<section><h2>MQTT</h2><div class='grid'>";
  page += "<div><label>Host</label><input name='mqtt_host' value='" + htmlEscape(defaults_.mqtt.host) + "'></div>";
  page += "<div><label>Port</label><input name='mqtt_port' type='number' value='" + String(defaults_.mqtt.port) + "'></div>";
  page += "<div><label>Client ID</label><input name='mqtt_client_id' value='" + htmlEscape(defaults_.mqtt.client_id) + "'></div>";
  page += "<div><label>Topic prefix</label><input name='mqtt_topic_prefix' value='" + htmlEscape(defaults_.mqtt.topic_prefix) + "'></div>";
  page += "<div><label>Username</label><input name='mqtt_username' value='" + htmlEscape(defaults_.mqtt.username) + "'></div>";
  page += "<div><label>Password</label><input name='mqtt_password' type='password' value='" + htmlEscape(defaults_.mqtt.password) + "'></div>";
  page += "</div></section>";

  auto renderOutputSection = [&](const char *title, const char *prefix, const OutputConfig &output) {
    page += "<section><h2>";
    page += title;
    page += "</h2><small>Driver values: gpio, shelly_http_rpc, kasa_local</small><div class='grid'>";
    page += "<div><label>Driver</label><input name='" + String(prefix) + "_driver' value='" + htmlEscape(output.driver) + "'></div>";
    page += "<div><label>GPIO pin</label><input name='" + String(prefix) + "_pin' type='number' value='" + String(output.pin) + "'></div>";
    page += "<div><label>GPIO active level</label><input name='" + String(prefix) + "_active_level' value='" + htmlEscape(output.active_level) + "'></div>";
    page += "<div><label>Host</label><input name='" + String(prefix) + "_host' value='" + htmlEscape(output.host) + "'></div>";
    page += "<div><label>Port</label><input name='" + String(prefix) + "_port' type='number' value='" + String(output.port) + "'></div>";
    page += "<div><label>Switch ID</label><input name='" + String(prefix) + "_switch_id' type='number' value='" + String(output.switch_id) + "'></div>";
    page += "<div><label>Username</label><input name='" + String(prefix) + "_username' value='" + htmlEscape(output.username) + "'></div>";
    page += "<div><label>Password</label><input name='" + String(prefix) + "_password' type='password' value='" + htmlEscape(output.password) + "'></div>";
    page += "<div><label>Alias</label><input name='" + String(prefix) + "_alias' value='" + htmlEscape(output.alias) + "'></div>";
    page += "</div></section>";
  };

  renderOutputSection("Heating output", "heating", defaults_.heating);
  renderOutputSection("Cooling output", "cooling", defaults_.cooling);
  page += "<button type='submit'>Save and reboot</button></form></main></body></html>";
  return page;
}

void ProvisioningServer::handleRoot() {
  server_.send(200, "text/html", renderPage());
}

void ProvisioningServer::handleSave() {
  SystemConfig config = buildFormConfig();
  String error;
  if (!validateSystemConfig(config, error)) {
    last_message_ = "Validation failed: " + error;
    defaults_ = config;
    server_.send(400, "text/html", renderPage());
    return;
  }
  pending_ = config;
  defaults_ = config;
  has_pending_ = true;
  last_message_ = "Configuration saved. Device will reboot into normal mode.";
  server_.send(200, "text/html", renderPage());
}

void ProvisioningServer::handleApiSave() {
  SystemConfig config;
  String error;
  if (!parseSystemConfigJson(server_.arg("plain"), config, error)) {
    server_.send(400, "application/json", "{\"result\":\"error\",\"message\":\"" + error + "\"}");
    return;
  }
  pending_ = config;
  defaults_ = config;
  has_pending_ = true;
  last_message_ = "Configuration saved over API. Device will reboot into normal mode.";
  server_.send(200, "application/json", "{\"result\":\"ok\"}");
}

void ProvisioningServer::handleNotFound() {
  server_.send(404, "text/plain", "Not found");
}
