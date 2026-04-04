#include "kasa_output_driver.h"

#include <ArduinoJson.h>
#include <WiFi.h>

KasaOutputDriver::KasaOutputDriver(const OutputConfig &config, const String &role) : config_(config), role_(role) {}

bool KasaOutputDriver::begin() {
  if (config_.port == 0) {
    config_.port = 9999;
  }
  status_.known = false;
  status_.on = false;
  updateDescription();
  if (WiFi.status() != WL_CONNECTED) {
    return true;
  }
  return refresh();
}

bool KasaOutputDriver::setState(bool on) {
  if (WiFi.status() != WL_CONNECTED || config_.host.isEmpty()) {
    status_.known = false;
    updateDescription();
    return false;
  }
  String response;
  const String command = "{\"system\":{\"set_relay_state\":{\"state\":" + String(on ? 1 : 0) + "}}}";
  if (!sendCommand(command, response)) {
    status_.known = false;
    updateDescription();
    return false;
  }
  status_.known = true;
  status_.on = on;
  updateDescription();
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
    return false;
  }

  DynamicJsonDocument doc(1024);
  if (deserializeJson(doc, response)) {
    status_.known = false;
    updateDescription();
    return false;
  }

  JsonVariant relay_state = doc["system"]["get_sysinfo"]["relay_state"];
  if (!relay_state.is<int>()) {
    status_.known = false;
    updateDescription();
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
  if (!client.connect(config_.host.c_str(), config_.port == 0 ? 9999 : config_.port)) {
    return false;
  }
  client.setTimeout(2500);

  const std::vector<uint8_t> payload = encryptPayload(command);
  if (client.write(payload.data(), payload.size()) != static_cast<int>(payload.size())) {
    client.stop();
    return false;
  }

  uint8_t buffer[1024];
  size_t read_bytes = 0;
  const uint32_t deadline = millis() + 2500;
  while (millis() < deadline) {
    while (client.available() && read_bytes < sizeof(buffer)) {
      buffer[read_bytes++] = static_cast<uint8_t>(client.read());
    }
    if (!client.connected() && !client.available()) {
      break;
    }
    delay(1);
  }
  client.stop();

  if (read_bytes <= 4) {
    return false;
  }
  response = decryptPayload(buffer + 4, read_bytes - 4);
  return !response.isEmpty();
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
