#include <Arduino.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <esp_system.h>
#include <time.h>

#include <memory>

#include "config_store.h"
#include "output_factory.h"
#include "provisioning_server.h"
#include "sensor_manager.h"
#include "thermostat_controller.h"

namespace {
constexpr char kFirmwareVersion[] = BREWESP_FW_VERSION;
constexpr int kServiceButtonPin = 33;
constexpr uint32_t kTelemetryIntervalMs = 15000;
constexpr uint32_t kStateRefreshIntervalMs = 300000;
constexpr uint32_t kOutputRefreshIntervalMs = 30000;
constexpr uint32_t kMqttReconnectIntervalMs = 5000;
constexpr uint32_t kWifiRetryIntervalMs = 500;
constexpr uint32_t kWifiReconnectIntervalMs = 10000;

ConfigStore g_store;
ProvisioningServer g_provisioning;
SensorManager g_sensors;
ThermostatController g_controller;
SystemConfig g_system_config = defaultSystemConfig();
FermentationConfig g_fermentation_config = defaultFermentationConfig(g_system_config.device_id);
std::unique_ptr<OutputDriver> g_heating_output;
std::unique_ptr<OutputDriver> g_cooling_output;
WiFiClient g_wifi_client;
PubSubClient g_mqtt_client(g_wifi_client);

bool g_provisioning_mode = false;
bool g_state_dirty = true;
uint32_t g_last_telemetry_ms = 0;
uint32_t g_last_state_ms = 0;
uint32_t g_last_heartbeat_ms = 0;
uint32_t g_last_output_refresh_ms = 0;
uint32_t g_last_mqtt_attempt_ms = 0;
uint32_t g_wifi_disconnect_started_ms = 0;
uint32_t g_last_wifi_reconnect_attempt_ms = 0;
bool g_system_patch_pending_reboot = false;
bool g_output_command_pending = false;
String g_pending_output_target;
String g_pending_output_state;

void publishState();

String topicBase() {
  return g_system_config.mqtt.topic_prefix + "/" + g_system_config.device_id;
}

String topicName(const char *suffix) {
  return topicBase() + "/" + suffix;
}

bool systemConfigChanged(const SystemConfig &current, const SystemConfig &updated) {
  return serializeSystemConfig(current) != serializeSystemConfig(updated);
}

time_t currentUnixTime() {
  const time_t now = time(nullptr);
  return now > 100000 ? now : 0;
}

void syncClock() {
  setenv("TZ", g_system_config.timezone.c_str(), 1);
  tzset();
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
}

IPAddress parseIpOrDefault(const String &value, const IPAddress &fallback) {
  IPAddress parsed;
  if (parsed.fromString(value)) {
    return parsed;
  }
  return fallback;
}

void rebuildOutputs() {
  g_heating_output = createOutputDriver(g_system_config.heating, "heating");
  g_cooling_output = createOutputDriver(g_system_config.cooling, "cooling");
  if (g_heating_output) {
    g_heating_output->begin();
  }
  if (g_cooling_output) {
    g_cooling_output->begin();
  }
  g_state_dirty = true;
}

void ensureOutputs() {
  if (!g_heating_output || !g_cooling_output) {
    rebuildOutputs();
  }
}

bool shouldAutoStartRecoveryAp(bool unprovisioned) {
  if (!g_system_config.recovery_ap.enabled) {
    return false;
  }
  if (unprovisioned && !g_system_config.recovery_ap.auto_start_when_unprovisioned) {
    return false;
  }
  return true;
}

void publishStateIfConnected() {
  if (g_mqtt_client.connected()) {
    publishState();
  }
}

bool shutOffForMutualExclusion(OutputDriver &output, const char *label) {
  const DriverStatus status = output.status();
  if (status.known && !status.on) {
    return true;
  }
  if (output.setState(false)) {
    return true;
  }
  Serial.printf("Refusing output handover because %s failed to turn off\n", label);
  return false;
}

void startProvisioningMode(const String &reason, bool reset_wifi) {
  if (g_provisioning_mode) {
    return;
  }
  g_provisioning_mode = true;
  if (reset_wifi) {
    WiFi.disconnect(true, false);
    delay(100);
  }
  WiFi.mode(WIFI_AP);
  const IPAddress ap_ip = parseIpOrDefault(g_system_config.recovery_ap.ip, IPAddress(192, 168, 4, 1));
  WiFi.softAPConfig(ap_ip, ap_ip, IPAddress(255, 255, 255, 0));
  WiFi.softAP(g_system_config.recovery_ap.ssid.c_str(), g_system_config.recovery_ap.password.c_str());
  g_provisioning.begin(g_system_config);
  Serial.printf("Recovery AP started: %s (%s) reason=%s\n", g_system_config.recovery_ap.ssid.c_str(),
                g_system_config.recovery_ap.ip.c_str(), reason.c_str());
}

bool connectWifiBlocking(uint32_t timeout_ms) {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, false);
  delay(100);

  if (!g_system_config.wifi.dhcp && !g_system_config.wifi.static_ip.isEmpty() && !g_system_config.wifi.gateway.isEmpty() &&
      !g_system_config.wifi.subnet.isEmpty()) {
    const IPAddress local = parseIpOrDefault(g_system_config.wifi.static_ip, IPAddress(0, 0, 0, 0));
    const IPAddress gateway = parseIpOrDefault(g_system_config.wifi.gateway, IPAddress(0, 0, 0, 0));
    const IPAddress subnet = parseIpOrDefault(g_system_config.wifi.subnet, IPAddress(255, 255, 255, 0));
    const IPAddress dns1 = parseIpOrDefault(g_system_config.wifi.dns1, gateway);
    const IPAddress dns2 = parseIpOrDefault(g_system_config.wifi.dns2, IPAddress(0, 0, 0, 0));
    WiFi.config(local, gateway, subnet, dns1, dns2);
  }

  WiFi.begin(g_system_config.wifi.ssid.c_str(), g_system_config.wifi.password.c_str());
  const uint32_t started = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - started) < timeout_ms) {
    delay(kWifiRetryIntervalMs);
  }
  if (WiFi.status() == WL_CONNECTED) {
    syncClock();
    Serial.printf("Wi-Fi connected: %s\n", WiFi.localIP().toString().c_str());
    return true;
  }
  return false;
}

void publishJson(const String &topic, DynamicJsonDocument &doc, bool retained) {
  String payload;
  serializeJson(doc, payload);
  g_mqtt_client.publish(topic.c_str(), payload.c_str(), retained);
}

void publishAvailability(const char *status) {
  if (!g_mqtt_client.connected()) {
    return;
  }
  DynamicJsonDocument doc(256);
  doc["device_id"] = g_system_config.device_id;
  doc["status"] = status;
  doc["fw_version"] = kFirmwareVersion;
  publishJson(topicName("availability"), doc, true);
}

void publishConfigApplied(const char *result, uint32_t requested_version, uint32_t applied_version, const String &message) {
  if (!g_mqtt_client.connected()) {
    return;
  }
  DynamicJsonDocument doc(256);
  doc["device_id"] = g_system_config.device_id;
  doc["requested_version"] = requested_version;
  doc["applied_version"] = applied_version;
  doc["result"] = result;
  if (message.isEmpty()) {
    doc["message"] = nullptr;
  } else {
    doc["message"] = message;
  }
  publishJson(topicName("config/applied"), doc, false);
}

void publishHeartbeat() {
  if (!g_mqtt_client.connected()) {
    return;
  }
  DynamicJsonDocument doc(384);
  doc["device_id"] = g_system_config.device_id;
  doc["uptime_s"] = millis() / 1000UL;
  doc["wifi_rssi"] = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
  doc["heap_free"] = ESP.getFreeHeap();
  doc["ui"] = "headless";
  doc["heating"] = outputStateString(g_heating_output->status());
  doc["cooling"] = outputStateString(g_cooling_output->status());
  publishJson(topicName("heartbeat"), doc, false);
}

void publishTelemetry() {
  if (!g_mqtt_client.connected()) {
    return;
  }
  const SensorSnapshot &snapshot = g_sensors.snapshot();
  const ControllerState &controller = g_controller.state();
  DynamicJsonDocument doc(1024);
  doc["device_id"] = g_system_config.device_id;
  doc["ts"] = currentUnixTime();
  doc["uptime_s"] = millis() / 1000UL;
  if (snapshot.beer.valid) {
    doc["temp_primary_c"] = snapshot.beer.adjusted_c;
  }
  if (snapshot.chamber.valid) {
    doc["temp_secondary_c"] = snapshot.chamber.adjusted_c;
  }
  if (g_fermentation_config.valid) {
    doc["setpoint_c"] = g_fermentation_config.thermostat.setpoint_c;
  }
  doc["mode"] = g_fermentation_config.valid ? g_fermentation_config.mode : "thermostat";
  doc["controller_state"] = controller.controller_state;
  doc["controller_reason"] = controller.controller_reason;
  doc["automatic_control_active"] = controller.automatic_control_active;
  doc["active_config_version"] = g_fermentation_config.version;
  doc["secondary_sensor_enabled"] = g_fermentation_config.sensors.secondary_enabled;
  doc["control_sensor"] = g_fermentation_config.sensors.control_sensor;
  doc["beer_probe_present"] = snapshot.beer.present;
  doc["beer_probe_valid"] = snapshot.beer.valid;
  doc["beer_probe_stale"] = snapshot.beer.stale;
  doc["beer_probe_rom"] = snapshot.beer.rom;
  doc["chamber_probe_present"] = snapshot.chamber.present;
  doc["chamber_probe_valid"] = snapshot.chamber.valid;
  doc["chamber_probe_stale"] = snapshot.chamber.stale;
  doc["chamber_probe_rom"] = snapshot.chamber.rom;
  doc["heating"] = g_heating_output->status().on;
  doc["cooling"] = g_cooling_output->status().on;
  if (controller.fault.isEmpty()) {
    doc["fault"] = nullptr;
  } else {
    doc["fault"] = controller.fault;
  }
  publishJson(topicName("telemetry"), doc, false);
}

void publishState() {
  if (!g_mqtt_client.connected()) {
    return;
  }
  const SensorSnapshot &snapshot = g_sensors.snapshot();
  const ControllerState &controller = g_controller.state();
  const DriverStatus heating = g_heating_output->status();
  const DriverStatus cooling = g_cooling_output->status();

  DynamicJsonDocument doc(1536);
  doc["device_id"] = g_system_config.device_id;
  doc["ui"] = "headless";
  doc["mode"] = g_fermentation_config.valid ? g_fermentation_config.mode : "thermostat";
  if (g_fermentation_config.valid) {
    doc["setpoint_c"] = g_fermentation_config.thermostat.setpoint_c;
    doc["hysteresis_c"] = g_fermentation_config.thermostat.hysteresis_c;
    doc["cooling_delay_s"] = g_fermentation_config.thermostat.cooling_delay_s;
    doc["heating_delay_s"] = g_fermentation_config.thermostat.heating_delay_s;
  }
  doc["fw_version"] = kFirmwareVersion;
  doc["ota_status"] = "idle";
  doc["ota_channel"] = g_system_config.ota.channel;
  doc["ota_available"] = false;
  doc["ota_progress_pct"] = 0;
  doc["ota_reboot_pending"] = false;
  doc["heating"] = outputStateString(heating);
  doc["cooling"] = outputStateString(cooling);
  doc["heating_desc"] = heating.description;
  doc["cooling_desc"] = cooling.description;
  doc["controller_state"] = controller.controller_state;
  doc["controller_reason"] = controller.controller_reason;
  doc["automatic_control_active"] = controller.automatic_control_active;
  doc["active_config_version"] = g_fermentation_config.version;
  doc["secondary_sensor_enabled"] = g_fermentation_config.sensors.secondary_enabled;
  doc["control_sensor"] = g_fermentation_config.sensors.control_sensor;
  doc["beer_probe_present"] = snapshot.beer.present;
  doc["beer_probe_valid"] = snapshot.beer.valid;
  doc["beer_probe_stale"] = snapshot.beer.stale;
  doc["beer_probe_rom"] = snapshot.beer.rom;
  doc["chamber_probe_present"] = snapshot.chamber.present;
  doc["chamber_probe_valid"] = snapshot.chamber.valid;
  doc["chamber_probe_stale"] = snapshot.chamber.stale;
  doc["chamber_probe_rom"] = snapshot.chamber.rom;
  if (controller.fault.isEmpty()) {
    doc["fault"] = nullptr;
  } else {
    doc["fault"] = controller.fault;
  }
  publishJson(topicName("state"), doc, false);
}

void handleFermentationConfigMessage(const String &payload) {
  FermentationConfig candidate;
  String error;
  if (!parseFermentationConfigJson(payload, candidate, error) ||
      !validateFermentationConfig(candidate, g_system_config.device_id, error)) {
    publishConfigApplied("error", candidate.version, g_fermentation_config.version, error);
    return;
  }
  g_fermentation_config = candidate;
  g_store.saveFermentationConfig(g_fermentation_config);
  publishConfigApplied("ok", candidate.version, candidate.version, "");
  g_state_dirty = true;
}

void applyOutputCommand(const String &target, const String &state) {
  ensureOutputs();

  const bool turn_on = state == "on";
  const bool turn_off = state == "off";
  if (!turn_on && !turn_off) {
    Serial.printf("Ignoring set_output with invalid state=%s\n", state.c_str());
    return;
  }

  bool changed = false;
  if (target == "heating") {
    if (turn_on && !shutOffForMutualExclusion(*g_cooling_output, "cooling")) {
      g_heating_output->refresh();
      g_cooling_output->refresh();
      g_state_dirty = true;
      publishStateIfConnected();
      return;
    }
    changed = g_heating_output->setState(turn_on);
  } else if (target == "cooling") {
    if (turn_on && !shutOffForMutualExclusion(*g_heating_output, "heating")) {
      g_heating_output->refresh();
      g_cooling_output->refresh();
      g_state_dirty = true;
      publishStateIfConnected();
      return;
    }
    changed = g_cooling_output->setState(turn_on);
  } else if (target == "all" && turn_off) {
    const bool heating_changed = g_heating_output->setState(false);
    const bool cooling_changed = g_cooling_output->setState(false);
    changed = heating_changed || cooling_changed;
  } else {
    Serial.printf("Ignoring unsupported set_output target=%s state=%s\n", target.c_str(), state.c_str());
    return;
  }

  g_heating_output->refresh();
  g_cooling_output->refresh();
  g_state_dirty = true;
  Serial.printf("Handled set_output target=%s state=%s changed=%s\n", target.c_str(), state.c_str(),
                changed ? "true" : "false");
  publishStateIfConnected();
}

void handleCommandMessage(const String &payload) {
  DynamicJsonDocument doc(512);
  if (deserializeJson(doc, payload)) {
    Serial.printf("Ignoring invalid command payload=%s\n", payload.c_str());
    return;
  }

  const String command = String(doc["command"] | "");
  if (command == "set_output") {
    g_pending_output_target = String(doc["target"] | "");
    g_pending_output_state = String(doc["state"] | "");
    g_output_command_pending = true;
    Serial.printf("Queued set_output target=%s state=%s\n", g_pending_output_target.c_str(),
                  g_pending_output_state.c_str());
    return;
  }

  if (command == "discover_kasa") {
    Serial.println("Ignoring discover_kasa in firmware_v2: not implemented yet");
    return;
  }

  Serial.printf("Ignoring unsupported command=%s payload=%s\n", command.c_str(), payload.c_str());
}

void processPendingCommands() {
  if (!g_output_command_pending) {
    return;
  }
  g_output_command_pending = false;
  applyOutputCommand(g_pending_output_target, g_pending_output_state);
}

void handleSystemPatchMessage(const String &payload) {
  SystemConfig updated;
  String error;
  if (!parseSystemConfigPatchJson(payload, g_system_config, updated, error)) {
    Serial.printf("Ignoring invalid system_config patch: %s payload=%s\n", error.c_str(), payload.c_str());
    return;
  }
  if (!systemConfigChanged(g_system_config, updated)) {
    Serial.println("Ignoring unchanged retained system_config patch");
    return;
  }
  g_system_config = updated;
  g_store.saveSystemConfig(g_system_config);
  Serial.println("Applied system_config patch, scheduling reboot");
  g_system_patch_pending_reboot = true;
}

void mqttCallback(char *topic, byte *payload, unsigned int length) {
  String topic_string(topic);
  String message;
  message.reserve(length);
  for (unsigned int index = 0; index < length; ++index) {
    message += static_cast<char>(payload[index]);
  }
  if (topic_string == topicName("config/desired")) {
    handleFermentationConfigMessage(message);
  } else if (topic_string == topicName("system_config")) {
    handleSystemPatchMessage(message);
  } else if (topic_string == topicName("command")) {
    handleCommandMessage(message);
  }
}

bool connectMqtt() {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }
  g_mqtt_client.setServer(g_system_config.mqtt.host.c_str(), g_system_config.mqtt.port);
  g_mqtt_client.setKeepAlive(g_system_config.mqtt.keepalive_s);
  g_mqtt_client.setBufferSize(4096);
  g_mqtt_client.setCallback(mqttCallback);

  DynamicJsonDocument will_doc(256);
  will_doc["device_id"] = g_system_config.device_id;
  will_doc["status"] = "offline";
  will_doc["fw_version"] = kFirmwareVersion;
  String will_payload;
  serializeJson(will_doc, will_payload);

  const char *mqtt_user = g_system_config.mqtt.username.isEmpty() ? nullptr : g_system_config.mqtt.username.c_str();
  const char *mqtt_pass = g_system_config.mqtt.username.isEmpty() ? nullptr : g_system_config.mqtt.password.c_str();

  const bool connected = g_mqtt_client.connect(g_system_config.mqtt.client_id.c_str(), mqtt_user, mqtt_pass,
                                               topicName("availability").c_str(), 1, true, will_payload.c_str());
  if (!connected) {
    return false;
  }

  g_mqtt_client.subscribe(topicName("config/desired").c_str(), 1);
  g_mqtt_client.subscribe(topicName("system_config").c_str(), 1);
  g_mqtt_client.subscribe(topicName("command").c_str(), 1);
  publishAvailability("online");
  g_state_dirty = true;
  return true;
}

void maintainConnectivity() {
  if (!g_provisioning_mode && WiFi.status() != WL_CONNECTED) {
    if (g_wifi_disconnect_started_ms == 0) {
      g_wifi_disconnect_started_ms = millis();
      g_last_wifi_reconnect_attempt_ms = 0;
    }
    if ((millis() - g_last_wifi_reconnect_attempt_ms) > kWifiReconnectIntervalMs) {
      g_last_wifi_reconnect_attempt_ms = millis();
      WiFi.reconnect();
    }
    if (g_system_config.recovery_ap.enabled &&
        (millis() - g_wifi_disconnect_started_ms) > g_system_config.recovery_ap.start_after_wifi_failure_s * 1000UL) {
      startProvisioningMode("wifi reconnect timeout", true);
    }
  } else if (WiFi.status() == WL_CONNECTED) {
    g_wifi_disconnect_started_ms = 0;
    g_last_wifi_reconnect_attempt_ms = 0;
  }

  if (g_provisioning_mode || WiFi.status() != WL_CONNECTED) {
    return;
  }
  if (g_mqtt_client.connected()) {
    g_mqtt_client.loop();
    return;
  }
  if ((millis() - g_last_mqtt_attempt_ms) < kMqttReconnectIntervalMs) {
    return;
  }
  g_last_mqtt_attempt_ms = millis();
  connectMqtt();
}

void setupControllerData() {
  if (!g_store.begin()) {
    Serial.println("Failed to open config store");
  }

  SystemConfig stored_system;
  if (g_store.loadSystemConfig(stored_system)) {
    g_system_config = stored_system;
  } else {
    g_system_config = defaultSystemConfig();
  }

  FermentationConfig stored_fermentation;
  if (g_store.loadFermentationConfig(stored_fermentation)) {
    String error;
    if (validateFermentationConfig(stored_fermentation, g_system_config.device_id, error)) {
      g_fermentation_config = stored_fermentation;
    } else {
      g_fermentation_config = defaultFermentationConfig(g_system_config.device_id);
    }
  } else {
    g_fermentation_config = defaultFermentationConfig(g_system_config.device_id);
  }
}
}  // namespace

void setup() {
  Serial.begin(115200);
  delay(200);
  pinMode(kServiceButtonPin, INPUT_PULLUP);

  setupControllerData();
  g_sensors.begin(g_system_config.sensors.onewire_gpio);

  const bool force_recovery = digitalRead(kServiceButtonPin) == LOW;
  if (force_recovery) {
    ensureOutputs();
    startProvisioningMode("service button held at boot", true);
    return;
  }

  if (!g_system_config.valid) {
    ensureOutputs();
    if (shouldAutoStartRecoveryAp(true)) {
      startProvisioningMode("missing system_config", true);
    } else {
      Serial.println("Missing system_config but recovery AP auto-start is disabled");
    }
    return;
  }

  if (!connectWifiBlocking(15000)) {
    ensureOutputs();
    if (shouldAutoStartRecoveryAp(false)) {
      startProvisioningMode("initial wifi connection failed", true);
    } else {
      Serial.println("Initial Wi-Fi connection failed and recovery AP is disabled");
    }
    return;
  }
  ensureOutputs();
}

void loop() {
  if (g_provisioning_mode) {
    g_provisioning.loop();
    SystemConfig updated;
    if (g_provisioning.consumePendingConfig(updated)) {
      g_system_config = updated;
      g_store.saveSystemConfig(g_system_config);
      delay(500);
      ESP.restart();
    }
  }

  processPendingCommands();
  ensureOutputs();
  g_sensors.tick(g_system_config, g_fermentation_config, millis());
  g_controller.evaluate(g_fermentation_config, g_sensors.snapshot(), millis());
  g_controller.apply(*g_heating_output, *g_cooling_output, millis());

  if ((millis() - g_last_output_refresh_ms) > kOutputRefreshIntervalMs) {
    g_last_output_refresh_ms = millis();
    g_heating_output->refresh();
    g_cooling_output->refresh();
    g_state_dirty = true;
  }

  maintainConnectivity();

  if (g_mqtt_client.connected()) {
    if ((millis() - g_last_heartbeat_ms) > g_system_config.heartbeat.interval_s * 1000UL) {
      g_last_heartbeat_ms = millis();
      publishHeartbeat();
    }
    if ((millis() - g_last_telemetry_ms) > kTelemetryIntervalMs) {
      g_last_telemetry_ms = millis();
      publishTelemetry();
    }
    if (g_state_dirty || (millis() - g_last_state_ms) > kStateRefreshIntervalMs) {
      g_last_state_ms = millis();
      g_state_dirty = false;
      publishState();
    }
  }

  if (g_system_patch_pending_reboot) {
    delay(250);
    ESP.restart();
  }

  delay(10);
}
