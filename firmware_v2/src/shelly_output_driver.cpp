#include "shelly_output_driver.h"

#include <ArduinoJson.h>
#include <WiFi.h>
#include <WiFiClient.h>

ShellyOutputDriver::ShellyOutputDriver(const OutputConfig &config, const String &role) : config_(config), role_(role) {}

bool ShellyOutputDriver::begin() {
  status_.known = false;
  status_.on = false;
  updateDescription();
  if (WiFi.status() != WL_CONNECTED) {
    return true;
  }
  return refresh();
}

bool ShellyOutputDriver::setState(bool on) {
  if (config_.https || WiFi.status() != WL_CONNECTED || config_.host.isEmpty()) {
    status_.known = false;
    updateDescription();
    return false;
  }
  String response;
  const String path = "/rpc/Switch.Set?id=" + String(config_.switch_id) + "&on=" + String(on ? "true" : "false");
  if (!performGet(path, response)) {
    status_.known = false;
    updateDescription();
    return false;
  }
  status_.known = true;
  status_.on = on;
  updateDescription();
  return true;
}

bool ShellyOutputDriver::refresh() {
  if (config_.https || WiFi.status() != WL_CONNECTED || config_.host.isEmpty()) {
    status_.known = false;
    updateDescription();
    return false;
  }
  String response;
  const String path = "/rpc/Switch.GetStatus?id=" + String(config_.switch_id);
  if (!performGet(path, response)) {
    status_.known = false;
    updateDescription();
    return false;
  }

  DynamicJsonDocument doc(512);
  if (deserializeJson(doc, response)) {
    status_.known = false;
    updateDescription();
    return false;
  }
  status_.known = doc["output"].is<bool>();
  status_.on = doc["output"] | false;
  updateDescription();
  return status_.known;
}

DriverStatus ShellyOutputDriver::status() const {
  return status_;
}

bool ShellyOutputDriver::performGet(const String &path, String &response) {
  WiFiClient client;
  HTTPClient http;
  const String url = "http://" + config_.host + ":" + String(config_.port == 0 ? 80 : config_.port) + path;
  if (!http.begin(client, url)) {
    return false;
  }
  http.setConnectTimeout(2500);
  http.setTimeout(2500);
  if (!config_.username.isEmpty()) {
    http.setAuthorization(config_.username.c_str(), config_.password.c_str());
  }
  const int code = http.GET();
  if (code <= 0 || code >= 300) {
    http.end();
    return false;
  }
  response = http.getString();
  http.end();
  return true;
}

void ShellyOutputDriver::updateDescription() {
  const String state = config_.https ? "https unsupported" : outputStateString(status_);
  status_.description = "shelly " + config_.host + " " + state;
}
