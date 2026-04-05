#include <Arduino.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ctype.h>
#include <esp_system.h>
#include <math.h>
#include <stdlib.h>
#include <time.h>

#include <memory>

#include "config_store.h"
#include "output_factory.h"
#include "profile_runtime.h"
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
constexpr uint8_t kLocalProfileEditorMaxSteps = 7;

ConfigStore g_store;
ProvisioningServer g_provisioning;
SensorManager g_sensors;
ThermostatController g_controller;
ProfileRuntimeManager g_profile_runtime;
SystemConfig g_system_config = defaultSystemConfig();
FermentationConfig g_fermentation_config = defaultFermentationConfig(g_system_config.device_id);
std::unique_ptr<OutputDriver> g_heating_output;
std::unique_ptr<OutputDriver> g_cooling_output;
WiFiClient g_wifi_client;
PubSubClient g_mqtt_client(g_wifi_client);
WebServer g_local_server(80);

bool g_provisioning_mode = false;
bool g_local_server_started = false;
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
bool g_profile_command_pending = false;
String g_pending_profile_command;
String g_pending_profile_step_id;

void publishState();
void publishAvailability(const char *status);
bool connectMqtt();

bool mqttConfigured(const SystemConfig &config) {
  return !config.mqtt.host.isEmpty() && !config.mqtt.topic_prefix.isEmpty();
}

bool externalConfigOwnerActive() {
  return isExternalConfigOwner(g_system_config.config_owner);
}

bool localConfigWritable() {
  return !externalConfigOwnerActive();
}

bool mqttRuntimeEnabled() {
  return externalConfigOwnerActive() && mqttConfigured(g_system_config);
}

String htmlEscape(String value) {
  value.replace("&", "&amp;");
  value.replace("<", "&lt;");
  value.replace(">", "&gt;");
  value.replace("\"", "&quot;");
  value.replace("'", "&#39;");
  return value;
}

String selectedAttr(bool selected) {
  return selected ? " selected" : "";
}

String checkedAttr(bool checked) {
  return checked ? " checked" : "";
}

String disabledAttr(bool disabled) {
  return disabled ? " disabled" : "";
}

uint32_t nextLocalConfigVersion() {
  return g_fermentation_config.version == 0 ? 1 : g_fermentation_config.version + 1;
}

bool storedProfileAvailable() {
  return g_fermentation_config.profile.step_count > 0 && !g_fermentation_config.profile.id.isEmpty();
}

bool localEditorStepEnabled(uint8_t index) {
  if (g_fermentation_config.profile.step_count == 0) {
    return index == 0;
  }
  return index < g_fermentation_config.profile.step_count && index < kLocalProfileEditorMaxSteps;
}

String formatFloatInput(float value, uint8_t decimals = 2) {
  char buffer[32];
  snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
  String text(buffer);
  while (text.endsWith("0")) {
    text.remove(text.length() - 1);
  }
  if (text.endsWith(".")) {
    text.remove(text.length() - 1);
  }
  return text;
}

String formatHoursInput(uint32_t duration_s) {
  if (duration_s == 0) {
    return "";
  }
  return formatFloatInput(static_cast<float>(duration_s) / 3600.0f, 2);
}

bool parseStrictFloat(String value, float &result) {
  value.trim();
  if (value.isEmpty()) {
    return false;
  }

  char *end = nullptr;
  result = strtof(value.c_str(), &end);
  while (end != nullptr && *end != '\0' && isspace(static_cast<unsigned char>(*end))) {
    ++end;
  }
  return end != value.c_str() && end != nullptr && *end == '\0' && isfinite(result);
}

bool parseStrictUint32(String value, uint32_t &result) {
  value.trim();
  if (value.isEmpty() || value[0] == '-') {
    return false;
  }

  char *end = nullptr;
  const unsigned long parsed = strtoul(value.c_str(), &end, 10);
  while (end != nullptr && *end != '\0' && isspace(static_cast<unsigned char>(*end))) {
    ++end;
  }
  if (end == value.c_str() || end == nullptr || *end != '\0') {
    return false;
  }

  result = static_cast<uint32_t>(parsed);
  return true;
}

bool parseDurationHours(String value, const String &field_label, uint32_t &seconds, String &error) {
  value.trim();
  if (value.isEmpty()) {
    seconds = 0;
    return true;
  }

  float hours = 0.0f;
  if (!parseStrictFloat(value, hours) || hours < 0.0f) {
    error = field_label + " must be a non-negative number of hours";
    return false;
  }

  const double total_seconds = static_cast<double>(hours) * 3600.0;
  if (total_seconds > 3596400.0) {
    error = field_label + " must be <= 999 hours";
    return false;
  }

  seconds = static_cast<uint32_t>(total_seconds + 0.5);
  return true;
}

String slugifyIdentifier(String value, const String &fallback) {
  value.trim();
  String slug;
  slug.reserve(value.length());
  bool pending_dash = false;

  for (size_t index = 0; index < value.length(); ++index) {
    const char raw = value[index];
    const unsigned char current = static_cast<unsigned char>(raw);
    if (isalnum(current)) {
      if (pending_dash && !slug.isEmpty() && !slug.endsWith("-")) {
        slug += '-';
      }
      slug += static_cast<char>(tolower(current));
      pending_dash = false;
      continue;
    }
    if (raw == '-' || raw == '_' || isspace(current)) {
      pending_dash = !slug.isEmpty();
    }
  }

  if (slug.endsWith("-")) {
    slug.remove(slug.length() - 1);
  }
  if (slug.isEmpty()) {
    slug = fallback;
  }
  return slug;
}

String profileStepId(const String &label, uint8_t index) {
  return slugifyIdentifier(label, "step") + "-" + String(index + 1);
}

void appendProfileRuntimeJson(JsonObject runtime, uint32_t now_ms) {
  runtime["active_profile_id"] = g_profile_runtime.state().active_profile_id;
  runtime["active_step_id"] = g_profile_runtime.state().active_step_id;
  runtime["active_step_index"] = g_profile_runtime.state().active_step_index;
  runtime["phase"] = g_profile_runtime.state().phase;
  runtime["step_started_at"] = g_profile_runtime.stepStartedAtSeconds(now_ms);
  if (g_profile_runtime.hasStepHoldStarted()) {
    runtime["step_hold_started_at"] = g_profile_runtime.stepHoldStartedAtSeconds(now_ms);
  } else {
    runtime["step_hold_started_at"] = nullptr;
  }
  runtime["effective_target_c"] = g_profile_runtime.effectiveTargetC(g_fermentation_config);
  runtime["waiting_for_manual_release"] = g_profile_runtime.state().waiting_for_manual_release;
  runtime["paused"] = g_profile_runtime.state().paused;
}

String topicBase() {
  return g_system_config.mqtt.topic_prefix + "/" + g_system_config.device_id;
}

String topicName(const char *suffix) {
  return topicBase() + "/" + suffix;
}

bool systemConfigChanged(const SystemConfig &current, const SystemConfig &updated) {
  return serializeSystemConfig(current) != serializeSystemConfig(updated);
}

bool fermentationConfigChanged(const FermentationConfig &current, const FermentationConfig &updated) {
  return serializeFermentationConfig(current) != serializeFermentationConfig(updated);
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

float publishedSetpointC() {
  if (!g_fermentation_config.valid) {
    return g_fermentation_config.thermostat.setpoint_c;
  }
  if (g_fermentation_config.mode == "profile" && g_profile_runtime.active()) {
    return g_profile_runtime.activeStepTargetC(g_fermentation_config);
  }
  return g_fermentation_config.thermostat.setpoint_c;
}

bool controlSensorOperational(const SensorSnapshot &snapshot, const FermentationConfig &config) {
  const ProbeReading &probe =
      (config.sensors.control_sensor == "secondary" && config.sensors.secondary_enabled) ? snapshot.chamber : snapshot.beer;
  return probe.present && probe.valid && !probe.stale;
}

void syncProfileRuntime() {
  g_profile_runtime.syncToConfig(g_fermentation_config, g_store, millis());
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

bool saveLocalFermentationConfig(const FermentationConfig &candidate, const char *source, String &error) {
  if (!localConfigWritable()) {
    error = "local config editing requires config_owner=local";
    return false;
  }

  FermentationConfig validated = candidate;
  if (!validateFermentationConfig(validated, g_system_config.device_id, error)) {
    return false;
  }

  const FermentationConfig previous = g_fermentation_config;
  g_fermentation_config = validated;
  if (!g_store.saveFermentationConfig(g_fermentation_config)) {
    g_fermentation_config = previous;
    error = "failed to persist fermentation_config";
    return false;
  }

  syncProfileRuntime();
  g_state_dirty = true;
  Serial.printf("Saved local fermentation_config via %s version=%lu mode=%s profile_steps=%u\n", source,
                static_cast<unsigned long>(g_fermentation_config.version), g_fermentation_config.mode.c_str(),
                g_fermentation_config.profile.step_count);
  publishStateIfConnected();
  return true;
}

bool buildLocalThermostatCandidateFromForm(FermentationConfig &candidate, bool activate_mode, String &error) {
  candidate = g_fermentation_config;
  candidate.schema_version = 2;
  candidate.device_id = g_system_config.device_id;
  candidate.version = nextLocalConfigVersion();

  if (!parseStrictFloat(g_local_server.arg("thermostat_setpoint_c"), candidate.thermostat.setpoint_c)) {
    error = "thermostat setpoint must be a number";
    return false;
  }
  if (candidate.thermostat.setpoint_c < -20.0f || candidate.thermostat.setpoint_c > 50.0f) {
    error = "thermostat setpoint must be between -20 and 50 C";
    return false;
  }
  if (!parseStrictFloat(g_local_server.arg("thermostat_hysteresis_c"), candidate.thermostat.hysteresis_c)) {
    error = "thermostat hysteresis must be a number";
    return false;
  }
  if (!parseStrictUint32(g_local_server.arg("thermostat_cooling_delay_s"), candidate.thermostat.cooling_delay_s)) {
    error = "cooling delay must be an integer number of seconds";
    return false;
  }
  if (!parseStrictUint32(g_local_server.arg("thermostat_heating_delay_s"), candidate.thermostat.heating_delay_s)) {
    error = "heating delay must be an integer number of seconds";
    return false;
  }
  if (candidate.thermostat.cooling_delay_s > 3600 || candidate.thermostat.heating_delay_s > 3600) {
    error = "thermostat delays must be <= 3600 seconds";
    return false;
  }

  if (activate_mode) {
    candidate.mode = "thermostat";
  }

  return validateFermentationConfig(candidate, g_system_config.device_id, error);
}

bool buildLocalProfileCandidateFromForm(FermentationConfig &candidate, String &error) {
  candidate = g_fermentation_config;
  candidate.schema_version = 2;
  candidate.device_id = g_system_config.device_id;
  candidate.version = nextLocalConfigVersion();

  String name = g_local_server.arg("name");
  name.trim();
  if (name.isEmpty()) {
    name = candidate.name;
  }
  if (name.isEmpty()) {
    name = "Local profile";
  }
  candidate.name = name;

  String profile_id = g_local_server.arg("profile_id");
  profile_id.trim();
  if (profile_id.isEmpty()) {
    profile_id = candidate.profile.id;
  }
  if (profile_id.isEmpty()) {
    profile_id = candidate.name;
  }

  candidate.profile = ProfileConfig();
  candidate.profile.id = slugifyIdentifier(profile_id, "local-profile");

  for (uint8_t index = 0; index < kLocalProfileEditorMaxSteps; ++index) {
    const String prefix = "step_" + String(index + 1) + "_";
    if (!g_local_server.hasArg(prefix + "enabled")) {
      continue;
    }

    if (candidate.profile.step_count >= kLocalProfileEditorMaxSteps) {
      error = "no more than 7 steps can be active";
      return false;
    }

    ProfileStepConfig &step = candidate.profile.steps[candidate.profile.step_count++];

    String label = g_local_server.arg(prefix + "label");
    label.trim();
    if (label.isEmpty()) {
      label = "Step " + String(index + 1);
    }
    step.label = label;
    step.id = profileStepId(label, index);

    const String target_field = prefix + "target_c";
    if (!parseStrictFloat(g_local_server.arg(target_field), step.target_c)) {
      error = "Step " + String(index + 1) + " target temperature must be a number";
      return false;
    }

    if (!parseDurationHours(g_local_server.arg(prefix + "hold_hours"),
                            "Step " + String(index + 1) + " hold duration", step.hold_duration_s, error)) {
      return false;
    }
    if (!parseDurationHours(g_local_server.arg(prefix + "ramp_hours"),
                            "Step " + String(index + 1) + " ramp duration", step.ramp_duration_s, error)) {
      return false;
    }

    step.advance_policy = g_local_server.arg(prefix + "advance_policy");
    step.advance_policy.trim();
    if (step.advance_policy != "auto" && step.advance_policy != "manual_release") {
      error = "Step " + String(index + 1) + " advance policy must be auto or manual_release";
      return false;
    }
  }

  if (candidate.profile.step_count == 0) {
    error = "select at least one active step";
    return false;
  }

  return validateFermentationConfig(candidate, g_system_config.device_id, error);
}

String localProfileCommandSuccessMessage(const String &command) {
  if (command == "profile_start") {
    return "Profile started from step 1.";
  }
  if (command == "profile_pause") {
    return "Profile paused.";
  }
  if (command == "profile_resume") {
    return "Profile resumed.";
  }
  if (command == "profile_release_hold") {
    return "Manual hold released.";
  }
  if (command == "profile_stop") {
    return "Profile stopped. Thermostat mode remains active with the last effective target.";
  }
  return "Profile command applied.";
}

bool applyLocalProfileCommand(const String &command, String &error) {
  if (!localConfigWritable()) {
    error = "local profile commands require config_owner=local";
    return false;
  }

  if (command == "profile_start") {
    if (!storedProfileAvailable()) {
      error = "save a local profile before starting it";
      return false;
    }

    FermentationConfig candidate = g_fermentation_config;
    candidate.device_id = g_system_config.device_id;
    candidate.mode = "profile";
    candidate.version = nextLocalConfigVersion();
    return saveLocalFermentationConfig(candidate, "local_http_profile_start", error);
  }

  if (command != "profile_pause" && command != "profile_resume" && command != "profile_release_hold" &&
      command != "profile_stop") {
    error = "unsupported local profile command";
    return false;
  }

  if (!g_profile_runtime.handleCommand(g_fermentation_config, command, "", g_store, millis())) {
    if (command == "profile_pause") {
      error = "profile_pause requires an active profile that is not already paused";
    } else if (command == "profile_resume") {
      error = "profile_resume requires a paused profile";
    } else if (command == "profile_release_hold") {
      error = "profile_release_hold requires a manual-release hold";
    } else if (command == "profile_stop") {
      error = "profile_stop requires profile mode";
    } else {
      error = "profile command failed";
    }
    return false;
  }

  g_state_dirty = true;
  Serial.printf("Handled local profile command=%s\n", command.c_str());
  publishStateIfConnected();
  return true;
}

String localControlPage(const String &message, bool is_error) {
  const bool writable = localConfigWritable();
  const bool runtime_active = g_fermentation_config.mode == "profile" && g_profile_runtime.active();
  const bool profile_too_large = g_fermentation_config.profile.step_count > kLocalProfileEditorMaxSteps;
  const bool can_start = writable && storedProfileAvailable();
  const bool can_pause = writable && runtime_active && !g_profile_runtime.state().paused &&
                         g_profile_runtime.state().phase != "completed";
  const bool can_resume = writable && runtime_active && g_profile_runtime.state().paused;
  const bool can_release = writable && runtime_active && g_profile_runtime.state().waiting_for_manual_release;
  const bool can_stop = writable && g_fermentation_config.mode == "profile";
  const DriverStatus heating = g_heating_output ? g_heating_output->status() : DriverStatus();
  const DriverStatus cooling = g_cooling_output ? g_cooling_output->status() : DriverStatus();
  const ControllerState &controller = g_controller.state();

  String page;
  page.reserve(16384);
  page += "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
  page += "<title>BrewESP Local Control</title><style>";
  page += "body{font-family:Segoe UI,Arial,sans-serif;margin:0;background:#f4f1ea;color:#1f2a30;}main{max-width:980px;margin:0 auto;padding:24px;}";
  page += "section{background:#fff;border-radius:14px;padding:18px;margin-bottom:16px;box-shadow:0 4px 14px rgba(0,0,0,.08);}h1,h2,h3{margin:0 0 12px;}p{margin:0 0 10px;line-height:1.45;}";
  page += ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:12px;}.card{background:#f9f6f0;border:1px solid #e3ddd2;border-radius:10px;padding:12px;}";
  page += "label{display:block;font-weight:600;margin:10px 0 6px;}input,select{width:100%;padding:10px;border:1px solid #c9ced3;border-radius:8px;box-sizing:border-box;background:#fff;}";
  page += "input[type='checkbox']{width:auto;padding:0;margin:0;accent-color:#1f6b75;}";
  page += "button{background:#1f6b75;color:#fff;border:0;padding:12px 18px;border-radius:8px;font-weight:700;cursor:pointer;}button[disabled],input[disabled],select[disabled]{opacity:.55;cursor:not-allowed;}";
  page += ".button-row{display:flex;flex-wrap:wrap;gap:10px;margin-top:12px;}.banner{padding:12px 14px;border-radius:8px;margin-bottom:16px;}.banner.ok{background:#dfeee8;border-left:4px solid #1f6b75;}.banner.error{background:#f9e2de;border-left:4px solid #a6432b;}";
  page += ".meta{color:#55636d;}.step-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));gap:12px;}.step{border:1px solid #d8d2c8;border-radius:12px;padding:14px;background:#fbfaf7;}.step.inactive{opacity:.55;}.help{font-size:.95rem;color:#55636d;}.step-toggle{display:flex;align-items:center;gap:8px;margin-bottom:10px;font-weight:700;}";
  page += "</style></head><body><main><section><h1>Local Control</h1>";
  if (!message.isEmpty()) {
    page += "<div class='banner " + String(is_error ? "error" : "ok") + "'>" + htmlEscape(message) + "</div>";
  }
  page += "<p class='meta'>Device: " + htmlEscape(g_system_config.device_id) + "<br>Current owner: " +
          htmlEscape(g_system_config.config_owner) +
          "<br>MQTT configured: " + String(mqttConfigured(g_system_config) ? "yes" : "no") +
          "<br>MQTT connected: " + String(g_mqtt_client.connected() ? "yes" : "no") +
          "<br>Current mode: " + htmlEscape(g_fermentation_config.mode) +
          "<br>Stored profile steps: " + String(g_fermentation_config.profile.step_count) +
          "<br>Active config version: " + String(g_fermentation_config.version) + "</p></section>";

  page += "<section><h2>Runtime Status</h2><div class='grid'>";
  page += "<div class='card'><strong>Outputs</strong><p class='meta'>Heater: " + htmlEscape(outputStateString(heating)) +
          "<br>Heater detail: " + htmlEscape(heating.description.isEmpty() ? String("n/a") : heating.description) +
          "<br>Cooler: " + htmlEscape(outputStateString(cooling)) +
          "<br>Cooler detail: " + htmlEscape(cooling.description.isEmpty() ? String("n/a") : cooling.description) +
          "</p></div>";
  page += "<div class='card'><strong>Controller</strong><p class='meta'>State: " + htmlEscape(controller.controller_state) +
          "<br>Reason: " + htmlEscape(controller.controller_reason);
  if (!controller.fault.isEmpty()) {
    page += "<br>Fault: " + htmlEscape(controller.fault);
  }
  page += "</p></div></div></section>";

  page += "<section><h2>Config Owner</h2><p class='help'>Profile editing and local run commands are only writable while ownership is <code>local</code>.</p>";
  page += "<form method='post' action='/control-owner'><label>Config owner</label>";
  page += "<select name='owner'><option value='local' " + String(g_system_config.config_owner == kConfigOwnerLocal ? "selected" : "") +
          ">local</option><option value='external' " +
          String(g_system_config.config_owner == kConfigOwnerExternal ? "selected" : "") + ">external</option></select>";
  page += "<div class='button-row'><button type='submit'>Apply without reboot</button></div></form></section>";

  page += "<section><h2>Profile Runtime</h2><div class='grid'>";
  page += "<div class='card'><strong>Stored profile</strong><p class='meta'>ID: " +
          htmlEscape(storedProfileAvailable() ? g_fermentation_config.profile.id : String("none")) +
          "<br>Name: " + htmlEscape(g_fermentation_config.name.isEmpty() ? String("unnamed") : g_fermentation_config.name) +
          "<br>Current mode: " + htmlEscape(g_fermentation_config.mode) + "</p></div>";
  page += "<div class='card'><strong>Runtime</strong><p class='meta'>";
  if (runtime_active) {
    page += "Phase: " + htmlEscape(g_profile_runtime.state().phase) +
            "<br>Active step: " + String(g_profile_runtime.state().active_step_index + 1) +
            "<br>Effective target: " + formatFloatInput(g_profile_runtime.effectiveTargetC(g_fermentation_config), 2) + " C" +
            "<br>Waiting manual release: " + String(g_profile_runtime.state().waiting_for_manual_release ? "yes" : "no");
  } else {
    page += "No active local profile runtime.";
  }
  page += "</p></div></div>";
  page += "<p class='help'>Start restarts the stored profile from step 1. Stop returns to thermostat mode but keeps the stored profile saved on the device.</p>";
  page += "<form method='post' action='/profile/command'><div class='button-row'>";
  page += "<button type='submit' name='command' value='profile_start'" + disabledAttr(!can_start) + ">Start profile</button>";
  page += "<button type='submit' name='command' value='profile_pause'" + disabledAttr(!can_pause) + ">Pause</button>";
  page += "<button type='submit' name='command' value='profile_resume'" + disabledAttr(!can_resume) + ">Resume</button>";
  page += "<button type='submit' name='command' value='profile_release_hold'" + disabledAttr(!can_release) + ">Release hold</button>";
  page += "<button type='submit' name='command' value='profile_stop'" + disabledAttr(!can_stop) + ">Stop</button>";
  page += "</div></form></section>";

  page += "<section><h2>Thermostat</h2>";
  page += "<p class='help'>Save thermostat settings locally. Use the second button to switch the controller back to thermostat mode.</p>";
  page += "<form method='post' action='/thermostat/save'><div class='grid'>";
  page += "<div><label>Setpoint (C)</label><input name='thermostat_setpoint_c' type='number' step='0.1' value='" +
          htmlEscape(formatFloatInput(g_fermentation_config.thermostat.setpoint_c, 2)) + "'" + disabledAttr(!writable) + "></div>";
  page += "<div><label>Hysteresis (C)</label><input name='thermostat_hysteresis_c' type='number' min='0.1' max='5' step='0.1' value='" +
          htmlEscape(formatFloatInput(g_fermentation_config.thermostat.hysteresis_c, 2)) + "'" + disabledAttr(!writable) + "></div>";
  page += "<div><label>Cooling delay (s)</label><input name='thermostat_cooling_delay_s' type='number' min='0' max='3600' step='1' value='" +
          String(g_fermentation_config.thermostat.cooling_delay_s) + "'" + disabledAttr(!writable) + "></div>";
  page += "<div><label>Heating delay (s)</label><input name='thermostat_heating_delay_s' type='number' min='0' max='3600' step='1' value='" +
          String(g_fermentation_config.thermostat.heating_delay_s) + "'" + disabledAttr(!writable) + "></div>";
  page += "</div><div class='button-row'>";
  page += "<button type='submit' name='action' value='save'" + disabledAttr(!writable) + ">Save thermostat settings</button>";
  page += "<button type='submit' name='action' value='activate'" + disabledAttr(!writable) + ">Save and switch to thermostat</button>";
  page += "</div></form></section>";

  page += "<section><h2>Profile Editor</h2>";
  if (!writable) {
    page += "<div class='banner error'>Switch ownership to <code>local</code> before saving or running a profile from the ESP.</div>";
  }
  if (profile_too_large) {
    page += "<div class='banner error'>The stored profile currently has more than 7 steps. This local editor shows the first 7 and saving will replace the stored profile with up to 7 steps.</div>";
  }
  page += "<p class='help'>This is a fallback editor, not the full web profile manager. Check <code>Active</code> on the steps you want to save; unchecked steps are ignored.</p>";
  page += "<form method='post' action='/profile/save'>";
  page += "<div class='grid'>";
  page += "<div><label>Profile name</label><input name='name' value='" +
          htmlEscape(g_fermentation_config.name.isEmpty() ? String("Local profile") : g_fermentation_config.name) + "'" +
          disabledAttr(!writable) + "></div>";
  page += "<div><label>Profile id</label><input name='profile_id' value='" +
          htmlEscape(g_fermentation_config.profile.id.isEmpty() ? String("local-profile") : g_fermentation_config.profile.id) + "'" +
          disabledAttr(!writable) + "></div></div>";
  page += "<div class='step-grid'>";
  for (uint8_t index = 0; index < kLocalProfileEditorMaxSteps; ++index) {
    ProfileStepConfig step;
    if (index < g_fermentation_config.profile.step_count) {
      step = g_fermentation_config.profile.steps[index];
    } else {
      step.label = "Step " + String(index + 1);
      step.target_c = index == 0 ? g_fermentation_config.thermostat.setpoint_c : 20.0f;
    }
    const String prefix = "step_" + String(index + 1) + "_";
    const bool active = localEditorStepEnabled(index);
    page += "<div class='step" + String(active ? "" : " inactive") + "'><h3>Step " + String(index + 1) + "</h3>";
    page += "<label class='step-toggle'><input type='checkbox' name='" + prefix + "enabled' value='1'" +
            checkedAttr(active) + disabledAttr(!writable) + ">Active</label>";
    page += "<label>Label</label><input name='" + prefix + "label' value='" + htmlEscape(step.label) + "'" +
            disabledAttr(!writable) + ">";
    page += "<label>Target temperature (C)</label><input name='" + prefix + "target_c' type='number' step='0.1' value='" +
            htmlEscape(formatFloatInput(step.target_c, 2)) + "'" + disabledAttr(!writable) + ">";
    page += "<label>Hold duration (hours)</label><input name='" + prefix + "hold_hours' type='number' min='0' step='0.25' value='" +
            htmlEscape(formatHoursInput(step.hold_duration_s)) + "'" + disabledAttr(!writable) + ">";
    page += "<label>Ramp duration (hours, optional)</label><input name='" + prefix + "ramp_hours' type='number' min='0' step='0.25' value='" +
            htmlEscape(formatHoursInput(step.ramp_duration_s)) + "'" + disabledAttr(!writable) + ">";
    page += "<label>Advance policy</label><select name='" + prefix + "advance_policy'" + disabledAttr(!writable) + ">";
    page += "<option value='auto'" + selectedAttr(step.advance_policy != "manual_release") + ">auto</option>";
    page += "<option value='manual_release'" + selectedAttr(step.advance_policy == "manual_release") + ">manual_release</option>";
    page += "</select></div>";
  }
  page += "</div><div class='button-row'><button type='submit'" + disabledAttr(!writable) + ">Save local profile</button></div></form></section>";
  page += "</main></body></html>";
  return page;
}

void sendLocalRuntimeState() {
  DynamicJsonDocument doc(4096);
  doc["device_id"] = g_system_config.device_id;
  doc["config_owner"] = g_system_config.config_owner;
  doc["local_config_writable"] = localConfigWritable();
  doc["external_config_active"] = externalConfigOwnerActive();
  doc["mqtt_configured"] = mqttConfigured(g_system_config);
  doc["mqtt_connected"] = g_mqtt_client.connected();
  doc["mode"] = g_fermentation_config.valid ? g_fermentation_config.mode : "thermostat";
  doc["active_config_version"] = g_fermentation_config.version;
  doc["name"] = g_fermentation_config.name;
  JsonObject thermostat = doc.createNestedObject("thermostat");
  thermostat["setpoint_c"] = g_fermentation_config.thermostat.setpoint_c;
  thermostat["hysteresis_c"] = g_fermentation_config.thermostat.hysteresis_c;
  thermostat["cooling_delay_s"] = g_fermentation_config.thermostat.cooling_delay_s;
  thermostat["heating_delay_s"] = g_fermentation_config.thermostat.heating_delay_s;
  doc["local_profile_editor_max_steps"] = kLocalProfileEditorMaxSteps;
  doc["stored_profile_available"] = storedProfileAvailable();
  doc["stored_profile_step_count"] = g_fermentation_config.profile.step_count;
  doc["stored_profile_truncated_for_local_editor"] = g_fermentation_config.profile.step_count > kLocalProfileEditorMaxSteps;
  if (storedProfileAvailable()) {
    JsonObject profile = doc.createNestedObject("profile");
    profile["id"] = g_fermentation_config.profile.id;
    JsonArray steps = profile.createNestedArray("steps");
    for (uint8_t index = 0; index < g_fermentation_config.profile.step_count; ++index) {
      const ProfileStepConfig &step = g_fermentation_config.profile.steps[index];
      JsonObject entry = steps.createNestedObject();
      entry["id"] = step.id;
      entry["label"] = step.label;
      entry["target_c"] = step.target_c;
      entry["hold_duration_s"] = step.hold_duration_s;
      entry["ramp_duration_s"] = step.ramp_duration_s;
      entry["advance_policy"] = step.advance_policy;
    }
  }
  if (g_fermentation_config.mode == "profile" && g_profile_runtime.active()) {
    JsonObject runtime = doc.createNestedObject("profile_runtime");
    appendProfileRuntimeJson(runtime, millis());
  }
  String payload;
  serializeJson(doc, payload);
  g_local_server.send(200, "application/json", payload);
}

void stopLocalControlServer() {
  if (!g_local_server_started) {
    return;
  }
  g_local_server.stop();
  g_local_server.close();
  g_local_server_started = false;
}

bool applyConfigOwnerChange(const String &requested_owner, const char *source, String &error) {
  if (!isValidConfigOwner(requested_owner)) {
    error = "owner must be local or external";
    return false;
  }
  if (requested_owner == g_system_config.config_owner) {
    return true;
  }

  SystemConfig previous = g_system_config;
  SystemConfig updated = g_system_config;
  updated.config_owner = requested_owner;
  if (!validateSystemConfig(updated, error)) {
    return false;
  }
  if (isExternalConfigOwner(updated.config_owner) && WiFi.status() != WL_CONNECTED) {
    error = "Wi-Fi must be connected before switching to external";
    return false;
  }

  if (g_mqtt_client.connected()) {
    publishAvailability("offline");
    g_mqtt_client.disconnect();
  }

  g_system_config = updated;
  if (isExternalConfigOwner(g_system_config.config_owner)) {
    g_last_mqtt_attempt_ms = 0;
    if (!connectMqtt()) {
      g_system_config = previous;
      if (isExternalConfigOwner(previous.config_owner) && mqttRuntimeEnabled()) {
        g_last_mqtt_attempt_ms = 0;
        connectMqtt();
      }
      error = "MQTT unavailable; stayed in " + previous.config_owner;
      return false;
    }
  }

  if (!g_store.saveSystemConfig(g_system_config)) {
    if (g_mqtt_client.connected()) {
      publishAvailability("offline");
      g_mqtt_client.disconnect();
    }
    g_system_config = previous;
    if (isExternalConfigOwner(previous.config_owner) && mqttRuntimeEnabled()) {
      g_last_mqtt_attempt_ms = 0;
      connectMqtt();
    }
    error = "failed to persist system_config";
    return false;
  }

  g_state_dirty = true;
  Serial.printf("Config owner switched via %s to %s\n", source, g_system_config.config_owner.c_str());
  publishStateIfConnected();
  return true;
}

void handleLocalControlRoot() {
  g_local_server.send(200, "text/html", localControlPage("", false));
}

void handleLocalControlOwnerForm() {
  String error;
  if (!applyConfigOwnerChange(g_local_server.arg("owner"), "local_http_form", error)) {
    g_local_server.send(409, "text/html", localControlPage(error, true));
    return;
  }
  g_local_server.send(200, "text/html", localControlPage("Config owner updated without reboot.", false));
}

void handleLocalControlOwnerApi() {
  DynamicJsonDocument doc(256);
  if (deserializeJson(doc, g_local_server.arg("plain"))) {
    g_local_server.send(400, "application/json", "{\"result\":\"error\",\"message\":\"invalid JSON\"}");
    return;
  }

  String error;
  const String owner = String(doc["owner"] | "");
  if (!applyConfigOwnerChange(owner, "local_http_api", error)) {
    g_local_server.send(409, "application/json",
                        "{\"result\":\"error\",\"message\":\"" + error + "\",\"config_owner\":\"" +
                            g_system_config.config_owner + "\"}");
    return;
  }

  g_local_server.send(200, "application/json",
                      "{\"result\":\"ok\",\"config_owner\":\"" + g_system_config.config_owner + "\"}");
}

void handleLocalProfileSaveForm() {
  String error;
  FermentationConfig candidate;
  if (!buildLocalProfileCandidateFromForm(candidate, error) ||
      !saveLocalFermentationConfig(candidate, "local_http_profile_save", error)) {
    g_local_server.send(409, "text/html", localControlPage(error, true));
    return;
  }

  g_local_server.send(200, "text/html", localControlPage("Local profile saved.", false));
}

void handleLocalThermostatSaveForm() {
  String error;
  FermentationConfig candidate;
  const bool activate_mode = g_local_server.arg("action") == "activate";
  if (!buildLocalThermostatCandidateFromForm(candidate, activate_mode, error) ||
      !saveLocalFermentationConfig(candidate, activate_mode ? "local_http_thermostat_activate" : "local_http_thermostat_save",
                                   error)) {
    g_local_server.send(409, "text/html", localControlPage(error, true));
    return;
  }

  g_local_server.send(200, "text/html",
                      localControlPage(activate_mode ? "Thermostat settings saved and thermostat mode activated."
                                                     : "Thermostat settings saved.",
                                       false));
}

void handleLocalProfileCommandForm() {
  String error;
  const String command = g_local_server.arg("command");
  if (!applyLocalProfileCommand(command, error)) {
    g_local_server.send(409, "text/html", localControlPage(error, true));
    return;
  }

  g_local_server.send(200, "text/html", localControlPage(localProfileCommandSuccessMessage(command), false));
}

void beginLocalControlServer() {
  if (g_local_server_started) {
    return;
  }
  g_local_server.on("/", HTTP_GET, handleLocalControlRoot);
  g_local_server.on("/control-owner", HTTP_POST, handleLocalControlOwnerForm);
  g_local_server.on("/thermostat/save", HTTP_POST, handleLocalThermostatSaveForm);
  g_local_server.on("/profile/save", HTTP_POST, handleLocalProfileSaveForm);
  g_local_server.on("/profile/command", HTTP_POST, handleLocalProfileCommandForm);
  g_local_server.on("/api/runtime/state", HTTP_GET, sendLocalRuntimeState);
  g_local_server.on("/api/control-owner", HTTP_POST, handleLocalControlOwnerApi);
  g_local_server.onNotFound([]() { g_local_server.send(404, "text/plain", "Not found"); });
  g_local_server.begin();
  g_local_server_started = true;
}

bool shutOffForMutualExclusion(OutputDriver &output, const char *label) {
  const DriverStatus status = output.status();
  if (status.known && !status.on) {
    return true;
  }
  if (output.setState(false)) {
    return true;
  }
  Serial.printf("Refusing output handover because %s failed to turn off (known=%s on=%s desc=%s)\n", label,
                status.known ? "true" : "false", status.on ? "true" : "false", status.description.c_str());
  return false;
}

void startProvisioningMode(const String &reason, bool reset_wifi) {
  if (g_provisioning_mode) {
    return;
  }
  g_provisioning_mode = true;
  stopLocalControlServer();
  if (g_mqtt_client.connected()) {
    publishAvailability("offline");
    g_mqtt_client.disconnect();
  }
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
  doc["config_owner"] = g_system_config.config_owner;
  doc["local_config_writable"] = localConfigWritable();
  if (snapshot.beer.valid) {
    doc["temp_primary_c"] = snapshot.beer.adjusted_c;
  }
  if (snapshot.chamber.valid) {
    doc["temp_secondary_c"] = snapshot.chamber.adjusted_c;
  }
  if (g_fermentation_config.valid) {
    doc["setpoint_c"] = publishedSetpointC();
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
  if (g_fermentation_config.mode == "profile" && g_profile_runtime.active()) {
    doc["profile_id"] = g_profile_runtime.state().active_profile_id;
    doc["profile_step_id"] = g_profile_runtime.state().active_step_id;
    doc["effective_target_c"] = g_profile_runtime.effectiveTargetC(g_fermentation_config);
  }
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
  doc["config_owner"] = g_system_config.config_owner;
  doc["local_config_writable"] = localConfigWritable();
  doc["external_config_active"] = externalConfigOwnerActive();
  doc["mode"] = g_fermentation_config.valid ? g_fermentation_config.mode : "thermostat";
  if (g_fermentation_config.valid) {
    doc["setpoint_c"] = publishedSetpointC();
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
  if (g_fermentation_config.mode == "profile" && g_profile_runtime.active()) {
    JsonObject runtime = doc.createNestedObject("profile_runtime");
    runtime["active_profile_id"] = g_profile_runtime.state().active_profile_id;
    runtime["active_step_id"] = g_profile_runtime.state().active_step_id;
    runtime["active_step_index"] = g_profile_runtime.state().active_step_index;
    runtime["phase"] = g_profile_runtime.state().phase;
    runtime["step_started_at"] = g_profile_runtime.stepStartedAtSeconds(millis());
    if (g_profile_runtime.hasStepHoldStarted()) {
      runtime["step_hold_started_at"] = g_profile_runtime.stepHoldStartedAtSeconds(millis());
    } else {
      runtime["step_hold_started_at"] = nullptr;
    }
    runtime["effective_target_c"] = g_profile_runtime.effectiveTargetC(g_fermentation_config);
    runtime["waiting_for_manual_release"] = g_profile_runtime.state().waiting_for_manual_release;
    runtime["paused"] = g_profile_runtime.state().paused;
  }
  if (controller.fault.isEmpty()) {
    doc["fault"] = nullptr;
  } else {
    doc["fault"] = controller.fault;
  }
  publishJson(topicName("state"), doc, false);
}

void handleFermentationConfigMessage(const String &payload) {
  if (!externalConfigOwnerActive()) {
    Serial.println("Ignoring config/desired because config_owner=local");
    return;
  }
  FermentationConfig candidate;
  String error;
  if (!parseFermentationConfigJson(payload, candidate, error) ||
      !validateFermentationConfig(candidate, g_system_config.device_id, error)) {
    publishConfigApplied("error", candidate.version, g_fermentation_config.version, error);
    return;
  }
  if (!fermentationConfigChanged(g_fermentation_config, candidate)) {
    publishConfigApplied("ok", candidate.version, g_fermentation_config.version, "");
    return;
  }
  g_fermentation_config = candidate;
  g_store.saveFermentationConfig(g_fermentation_config);
  syncProfileRuntime();
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

  if (g_fermentation_config.mode == "manual") {
    String next_output = g_fermentation_config.manual.output;
    if (target == "heating") {
      next_output = turn_on ? "heating" : "off";
    } else if (target == "cooling") {
      next_output = turn_on ? "cooling" : "off";
    } else if (target == "all" && turn_off) {
      next_output = "off";
    }

    if (next_output != g_fermentation_config.manual.output &&
        (next_output == "off" || next_output == "heating" || next_output == "cooling")) {
      g_fermentation_config.manual.output = next_output;
      g_store.saveFermentationConfig(g_fermentation_config);
      g_state_dirty = true;
    }
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
  Serial.printf("Handled set_output target=%s state=%s changed=%s heating=%s cooling=%s\n", target.c_str(),
                state.c_str(), changed ? "true" : "false", g_heating_output->status().description.c_str(),
                g_cooling_output->status().description.c_str());
  publishStateIfConnected();
}

void handleCommandMessage(const String &payload) {
  if (!externalConfigOwnerActive()) {
    Serial.println("Ignoring command because config_owner=local");
    return;
  }
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

  if (command == "profile_pause" || command == "profile_resume" || command == "profile_release_hold" ||
      command == "profile_jump_to_step" || command == "profile_stop") {
    JsonObjectConst args = doc["args"].as<JsonObjectConst>();
    g_pending_profile_command = command;
    g_pending_profile_step_id = String(args["step_id"] | "");
    g_profile_command_pending = true;
    Serial.printf("Queued profile command=%s step_id=%s\n", g_pending_profile_command.c_str(),
                  g_pending_profile_step_id.c_str());
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
  } else {
    g_output_command_pending = false;
    applyOutputCommand(g_pending_output_target, g_pending_output_state);
  }

  if (!g_profile_command_pending) {
    return;
  }
  g_profile_command_pending = false;
  if (g_profile_runtime.handleCommand(g_fermentation_config, g_pending_profile_command, g_pending_profile_step_id, g_store,
                                      millis())) {
    g_state_dirty = true;
  }
}

void handleSystemPatchMessage(const String &payload) {
  if (!externalConfigOwnerActive()) {
    Serial.println("Ignoring system_config because config_owner=local");
    return;
  }
  SystemConfig updated;
  String error;
  if (!parseSystemConfigPatchJson(payload, g_system_config, updated, error)) {
    Serial.printf("Ignoring invalid system_config patch: %s payload=%s\n", error.c_str(), payload.c_str());
    return;
  }
  if (updated.config_owner != g_system_config.config_owner) {
    Serial.println("Ignoring system_config patch that attempts to change config_owner");
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
  if (!mqttRuntimeEnabled()) {
    return false;
  }
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
  if (!mqttRuntimeEnabled()) {
    if (g_mqtt_client.connected()) {
      publishAvailability("offline");
      g_mqtt_client.disconnect();
    }
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

  syncProfileRuntime();
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
  beginLocalControlServer();
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

  if (g_local_server_started) {
    g_local_server.handleClient();
  }

  processPendingCommands();
  ensureOutputs();
  g_sensors.tick(g_system_config, g_fermentation_config, millis());
  const SensorSnapshot &snapshot = g_sensors.snapshot();
  if (g_profile_runtime.update(g_fermentation_config, controlSensorOperational(snapshot, g_fermentation_config), g_store,
                               millis())) {
    g_state_dirty = true;
  }
  g_controller.evaluate(g_fermentation_config, g_profile_runtime.state(), snapshot, millis());
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
