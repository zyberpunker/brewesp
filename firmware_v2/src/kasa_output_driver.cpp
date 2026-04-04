#include "kasa_output_driver.h"

#include <ArduinoJson.h>
#include <WiFi.h>

namespace {
constexpr uint32_t kKasaSocketTimeoutMs = 1500;
}

KasaOutputDriver::KasaOutputDriver(const OutputConfig &config, const String &role) : config_(config), role_(role) {}

bool KasaOutputDriver::begin() {
  if (config_.port == 0) {
    config_.port = 9999;
  }
  status_.known = false;
  status_.on = false;
  updateDescription();
  Serial.printf("[kasa:%s] init host=%s port=%u alias=%s poll_interval_s=%lu\n", role_.c_str(), config_.host.c_str(),
                config_.port, config_.alias.c_str(), static_cast<unsigned long>(config_.poll_interval_s));
  if (WiFi.status() != WL_CONNECTED) {
    return true;
  }
  return refresh();
}

bool KasaOutputDriver::setState(bool on) {
  if (WiFi.status() != WL_CONNECTED || config_.host.isEmpty()) {
    status_.known = false;
    updateDescription();
    Serial.printf("[kasa:%s] refusing setState host=%s wifi=%d empty_host=%s\n", role_.c_str(), config_.host.c_str(),
                  WiFi.status(), config_.host.isEmpty() ? "true" : "false");
    return false;
  }
  String response;
  const String command = "{\"system\":{\"set_relay_state\":{\"state\":" + String(on ? 1 : 0) + "}}}";
  if (!sendCommand(command, response)) {
    status_.known = false;
    updateDescription();
    Serial.printf("[kasa:%s] setState transport failed host=%s desired=%s\n", role_.c_str(), config_.host.c_str(),
                  on ? "on" : "off");
    return false;
  }
  const int err_code = parseErrCode(response);
  if (err_code != 0) {
    status_.known = false;
    updateDescription();
    Serial.printf("[kasa:%s] setState err_code=%d host=%s desired=%s response=%s\n", role_.c_str(), err_code,
                  config_.host.c_str(), on ? "on" : "off", response.c_str());
    return false;
  }
  delay(150);
  last_refresh_ms_ = 0;
  if (!refresh()) {
    Serial.printf("[kasa:%s] setState verify refresh failed host=%s desired=%s\n", role_.c_str(), config_.host.c_str(),
                  on ? "on" : "off");
    return false;
  }
  if (status_.on != on) {
    Serial.printf("[kasa:%s] setState verify mismatch host=%s desired=%s actual=%s\n", role_.c_str(),
                  config_.host.c_str(), on ? "on" : "off", status_.on ? "on" : "off");
    return false;
  }
  return true;
}

bool KasaOutputDriver::refresh() {
  if (WiFi.status() != WL_CONNECTED || config_.host.isEmpty()) {
    status_.known = false;
    updateDescription();
    return false;
  }
  if (millis() - last_refresh_ms_ < config_.poll_interval_s * 1000UL && status_.known) {
    return true;
  }
  last_refresh_ms_ = millis();

  String response;
  if (!sendCommand("{\"system\":{\"get_sysinfo\":{}}}", response)) {
    status_.known = false;
    updateDescription();
    Serial.printf("[kasa:%s] refresh transport failed host=%s\n", role_.c_str(), config_.host.c_str());
    return false;
  }
  const int err_code = parseErrCode(response);
  if (err_code != 0) {
    status_.known = false;
    updateDescription();
    Serial.printf("[kasa:%s] refresh err_code=%d host=%s response=%s\n", role_.c_str(), err_code, config_.host.c_str(),
                  response.c_str());
    return false;
  }

  DynamicJsonDocument doc(1024);
  if (deserializeJson(doc, response)) {
    status_.known = false;
    updateDescription();
    Serial.printf("[kasa:%s] refresh deserialize failed host=%s response=%s\n", role_.c_str(), config_.host.c_str(),
                  response.c_str());
    return false;
  }

  JsonVariant relay_state = doc["system"]["get_sysinfo"]["relay_state"];
  if (!relay_state.is<int>()) {
    status_.known = false;
    updateDescription();
    Serial.printf("[kasa:%s] refresh missing relay_state host=%s response=%s\n", role_.c_str(), config_.host.c_str(),
                  response.c_str());
    return false;
  }

  status_.known = true;
  status_.on = relay_state.as<int>() != 0;
  updateDescription();
  return true;
}

DriverStatus KasaOutputDriver::status() const {
  return status_;
}

bool KasaOutputDriver::sendCommand(const String &command, String &response) {
  WiFiClient client;
  client.setTimeout(kKasaSocketTimeoutMs);
  if (!client.connect(config_.host.c_str(), config_.port == 0 ? 9999 : config_.port)) {
    Serial.printf("[kasa:%s] connect failed host=%s port=%u\n", role_.c_str(), config_.host.c_str(),
                  config_.port == 0 ? 9999 : config_.port);
    return false;
  }

  const std::vector<uint8_t> payload = encryptPayload(command);
  if (client.write(payload.data(), payload.size()) != static_cast<int>(payload.size())) {
    client.stop();
    Serial.printf("[kasa:%s] short write host=%s expected=%u\n", role_.c_str(), config_.host.c_str(),
                  static_cast<unsigned>(payload.size()));
    return false;
  }

  uint8_t header[4];
  if (client.readBytes(header, sizeof(header)) != sizeof(header)) {
    client.stop();
    Serial.printf("[kasa:%s] header read failed host=%s\n", role_.c_str(), config_.host.c_str());
    return false;
  }

  const uint32_t response_length = (static_cast<uint32_t>(header[0]) << 24) | (static_cast<uint32_t>(header[1]) << 16) |
                                   (static_cast<uint32_t>(header[2]) << 8) | static_cast<uint32_t>(header[3]);
  std::vector<uint8_t> buffer(response_length);
  if (response_length > 0 &&
      client.readBytes(buffer.data(), response_length) != static_cast<int>(response_length)) {
    client.stop();
    Serial.printf("[kasa:%s] payload read failed host=%s expected=%lu\n", role_.c_str(), config_.host.c_str(),
                  static_cast<unsigned long>(response_length));
    return false;
  }
  client.stop();

  response = decryptPayload(buffer.data(), buffer.size());
  return true;
}

int KasaOutputDriver::parseErrCode(const String &response) {
  int err_index = response.indexOf("\"err_code\"");
  if (err_index < 0) {
    return 0;
  }

  err_index = response.indexOf(':', err_index);
  if (err_index < 0) {
    return 0;
  }

  ++err_index;
  while (err_index < response.length() && response[err_index] == ' ') {
    ++err_index;
  }

  int end_index = err_index;
  if (end_index < response.length() && response[end_index] == '-') {
    ++end_index;
  }
  while (end_index < response.length() && isDigit(response[end_index])) {
    ++end_index;
  }

  if (end_index == err_index) {
    return 0;
  }
  return response.substring(err_index, end_index).toInt();
}

std::vector<uint8_t> KasaOutputDriver::encryptPayload(const String &payload) {
  std::vector<uint8_t> buffer(payload.length() + 4);
  const uint32_t length = payload.length();
  buffer[0] = static_cast<uint8_t>((length >> 24) & 0xff);
  buffer[1] = static_cast<uint8_t>((length >> 16) & 0xff);
  buffer[2] = static_cast<uint8_t>((length >> 8) & 0xff);
  buffer[3] = static_cast<uint8_t>(length & 0xff);

  uint8_t key = 171;
  for (size_t index = 0; index < payload.length(); ++index) {
    buffer[index + 4] = static_cast<uint8_t>(payload[index]) ^ key;
    key = buffer[index + 4];
  }
  return buffer;
}

String KasaOutputDriver::decryptPayload(const uint8_t *buffer, size_t length) {
  String payload;
  payload.reserve(length);
  uint8_t key = 171;
  for (size_t index = 0; index < length; ++index) {
    const uint8_t value = buffer[index];
    payload += static_cast<char>(value ^ key);
    key = value;
  }
  return payload;
}

void KasaOutputDriver::updateDescription() {
  String label = config_.alias.isEmpty() ? config_.host : config_.alias;
  status_.description = "kasa " + label + " " + outputStateString(status_);
}
