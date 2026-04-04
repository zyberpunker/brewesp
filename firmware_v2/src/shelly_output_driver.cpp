#include "shelly_output_driver.h"

#include <ArduinoJson.h>
#include <WiFi.h>
#include <WiFiClient.h>

ShellyOutputDriver::ShellyOutputDriver(const OutputConfig &config, const String &role) : config_(config), role_(role) {}

bool ShellyOutputDriver::begin() {
  status_.known = false;
  status_.on = false;
  updateDescription();
  Serial.printf("[shelly:%s] init host=%s port=%u switch_id=%u https=%s\n", role_.c_str(), config_.host.c_str(),
                config_.port == 0 ? 80 : config_.port, config_.switch_id, config_.https ? "true" : "false");
  if (WiFi.status() != WL_CONNECTED) {
    return true;
  }
  return refresh();
}

bool ShellyOutputDriver::setState(bool on) {
  if (config_.https || WiFi.status() != WL_CONNECTED || config_.host.isEmpty()) {
    status_.known = false;
    updateDescription();
    Serial.printf("[shelly:%s] refusing setState host=%s wifi=%d https=%s empty_host=%s\n", role_.c_str(),
                  config_.host.c_str(), WiFi.status(), config_.https ? "true" : "false",
                  config_.host.isEmpty() ? "true" : "false");
    return false;
  }
  String response;
  const String path = "/rpc/Switch.Set?id=" + String(config_.switch_id) + "&on=" + String(on ? "true" : "false");
  if (!performGet(path, response)) {
    status_.known = false;
    updateDescription();
    Serial.printf("[shelly:%s] setState transport failed host=%s desired=%s\n", role_.c_str(), config_.host.c_str(),
                  on ? "on" : "off");
    return false;
  }
  delay(100);
  if (!refresh()) {
    Serial.printf("[shelly:%s] setState verify refresh failed host=%s desired=%s response=%s\n", role_.c_str(),
                  config_.host.c_str(), on ? "on" : "off", response.c_str());
    return false;
  }
  if (status_.on != on) {
    Serial.printf("[shelly:%s] setState verify mismatch host=%s desired=%s actual=%s response=%s\n", role_.c_str(),
                  config_.host.c_str(), on ? "on" : "off", status_.on ? "on" : "off", response.c_str());
    return false;
  }
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
    Serial.printf("[shelly:%s] refresh transport failed host=%s\n", role_.c_str(), config_.host.c_str());
    return false;
  }

  DynamicJsonDocument doc(512);
  if (deserializeJson(doc, response)) {
    status_.known = false;
    updateDescription();
    Serial.printf("[shelly:%s] refresh deserialize failed host=%s response=%s\n", role_.c_str(), config_.host.c_str(),
                  response.c_str());
    return false;
  }
  status_.known = doc["output"].is<bool>();
  status_.on = doc["output"] | false;
  updateDescription();
  if (!status_.known) {
    Serial.printf("[shelly:%s] refresh missing output field host=%s response=%s\n", role_.c_str(), config_.host.c_str(),
                  response.c_str());
  }
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
    Serial.printf("[shelly:%s] http begin failed url=%s\n", role_.c_str(), url.c_str());
    return false;
  }
  http.setConnectTimeout(2500);
  http.setTimeout(2500);
  if (!config_.username.isEmpty()) {
    http.setAuthorization(config_.username.c_str(), config_.password.c_str());
  }
  const int code = http.GET();
  if (code <= 0 || code >= 300) {
    Serial.printf("[shelly:%s] http GET failed url=%s code=%d\n", role_.c_str(), url.c_str(), code);
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
