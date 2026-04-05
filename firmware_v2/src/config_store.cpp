#include "config_store.h"

#include <ArduinoJson.h>
#include <Preferences.h>
#include <inttypes.h>

namespace {
Preferences g_preferences;
constexpr size_t kMaxStoredProfileRuntimeBytes = 768;

String chipSuffix() {
  const uint64_t chip_id = ESP.getEfuseMac();
  char buffer[9];
  snprintf(buffer, sizeof(buffer), "%08" PRIx32, static_cast<uint32_t>(chip_id & 0xffffffffULL));
  return String(buffer);
}

void writeOutputJson(JsonObject output, const OutputConfig &config) {
  output["driver"] = config.driver;
  if (config.driver == "gpio") {
    output["pin"] = config.pin;
    output["active_level"] = config.active_level;
  } else if (config.driver == "shelly_http_rpc") {
    output["host"] = config.host;
    output["port"] = config.port;
    output["switch_id"] = config.switch_id;
    output["username"] = config.username;
    output["password"] = config.password;
    output["https"] = config.https;
  } else if (config.driver == "kasa_local") {
    output["host"] = config.host;
    output["port"] = config.port;
    output["alias"] = config.alias;
    output["poll_interval_s"] = config.poll_interval_s;
  }
}

void parseOutputJson(JsonVariantConst source, OutputConfig &output) {
  if (!source.is<JsonObjectConst>()) {
    return;
  }

  JsonObjectConst object = source.as<JsonObjectConst>();
  output.driver = String(object["driver"] | output.driver);
  if (output.driver == "gpio") {
    output.pin = object["pin"] | output.pin;
    output.active_level = String(object["active_level"] | output.active_level);
  } else if (output.driver == "shelly_http_rpc") {
    output.host = String(object["host"] | output.host);
    output.port = object["port"] | output.port;
    output.switch_id = object["switch_id"] | output.switch_id;
    output.username = String(object["username"] | output.username);
    output.password = String(object["password"] | output.password);
    output.https = object["https"] | output.https;
    if (output.port == 0) {
      output.port = 80;
    }
  } else if (output.driver == "kasa_local") {
    output.host = String(object["host"] | output.host);
    output.port = object["port"] | output.port;
    output.alias = String(object["alias"] | output.alias);
    output.poll_interval_s = object["poll_interval_s"] | output.poll_interval_s;
    if (output.port == 0) {
      output.port = 9999;
    }
  }
}

bool validateOutput(OutputConfig &output, const char *label, String &error) {
  if (output.driver == "gpio") {
    if (output.pin < 0 || output.pin > 39) {
      error = String(label) + " gpio pin must be between 0 and 39";
      return false;
    }
    if (output.active_level != "high" && output.active_level != "low") {
      error = String(label) + " gpio active_level must be high or low";
      return false;
    }
    return true;
  }

  if (output.driver == "shelly_http_rpc") {
    if (output.host.isEmpty()) {
      error = String(label) + " shelly host is required";
      return false;
    }
    if (output.https) {
      error = String(label) + " shelly https is not supported in firmware_v2 yet";
      return false;
    }
    if (output.port == 0) {
      output.port = 80;
    }
    return true;
  }

  if (output.driver == "kasa_local") {
    if (output.host.isEmpty()) {
      error = String(label) + " kasa host is required";
      return false;
    }
    if (output.port == 0) {
      output.port = 9999;
    }
    if (output.poll_interval_s < 5) {
      output.poll_interval_s = 5;
    }
    return true;
  }

  error = String(label) + " driver must be gpio, shelly_http_rpc, or kasa_local";
  return false;
}
}  // namespace

bool ConfigStore::begin() {
  if (started_) {
    return true;
  }
  started_ = g_preferences.begin("brewesp-v2", false);
  return started_;
}

bool ConfigStore::loadSystemConfig(SystemConfig &config) {
  if (!begin()) {
    return false;
  }
  const String payload = g_preferences.getString("system_json", "");
  if (payload.isEmpty()) {
    return false;
  }
  String error;
  return parseSystemConfigJson(payload, config, error);
}

bool ConfigStore::saveSystemConfig(const SystemConfig &config) {
  if (!begin()) {
    return false;
  }
  return g_preferences.putString("system_json", serializeSystemConfig(config)) > 0;
}

bool ConfigStore::loadFermentationConfig(FermentationConfig &config) {
  if (!begin()) {
    return false;
  }
  const String payload = g_preferences.getString("ferment_json", "");
  if (payload.isEmpty()) {
    return false;
  }
  String error;
  return parseFermentationConfigJson(payload, config, error);
}

bool ConfigStore::saveFermentationConfig(const FermentationConfig &config) {
  if (!begin()) {
    return false;
  }
  return g_preferences.putString("ferment_json", serializeFermentationConfig(config)) > 0;
}

String defaultDeviceId() {
  return "brewesp-" + chipSuffix();
}

SystemConfig defaultSystemConfig() {
  SystemConfig config;
  config.device_id = defaultDeviceId();
  config.recovery_ap.ssid = "brewesp-setup-" + chipSuffix().substring(0, 6);
  config.mqtt.client_id = config.device_id;
  config.heating.driver = "gpio";
  config.heating.pin = 25;
  config.heating.active_level = "high";
  config.cooling.driver = "gpio";
  config.cooling.pin = 26;
  config.cooling.active_level = "high";
  return config;
}

FermentationConfig defaultFermentationConfig(const String &device_id) {
  FermentationConfig config;
  config.device_id = device_id;
  config.manual.output = "off";
  config.valid = false;
  return config;
}

bool parseSystemConfigJson(const String &payload, SystemConfig &config, String &error) {
  DynamicJsonDocument doc(4096);
  DeserializationError parse_error = deserializeJson(doc, payload);
  if (parse_error) {
    error = "system_config JSON parse failed";
    return false;
  }

  config = defaultSystemConfig();
  JsonObjectConst root = doc.as<JsonObjectConst>();
  config.schema_version = root["schema_version"] | config.schema_version;
  config.device_id = String(root["device_id"] | config.device_id);
  config.timezone = String(root["timezone"] | config.timezone);

  JsonObjectConst network = root["network"].as<JsonObjectConst>();
  JsonObjectConst wifi = network["wifi"].as<JsonObjectConst>();
  config.wifi.ssid = String(wifi["ssid"] | "");
  config.wifi.password = String(wifi["password"] | "");
  config.wifi.dhcp = wifi["dhcp"] | true;
  config.wifi.static_ip = String(wifi["static_ip"] | "");
  config.wifi.gateway = String(wifi["gateway"] | "");
  config.wifi.subnet = String(wifi["subnet"] | "");
  JsonArrayConst dns = wifi["dns"].as<JsonArrayConst>();
  if (dns.size() > 0) {
    config.wifi.dns1 = String(dns[0].as<const char *>());
  }
  if (dns.size() > 1) {
    config.wifi.dns2 = String(dns[1].as<const char *>());
  }

  JsonObjectConst recovery_ap = network["recovery_ap"].as<JsonObjectConst>();
  config.recovery_ap.enabled = recovery_ap["enabled"] | config.recovery_ap.enabled;
  config.recovery_ap.ssid = String(recovery_ap["ssid"] | config.recovery_ap.ssid);
  config.recovery_ap.password = String(recovery_ap["password"] | config.recovery_ap.password);
  config.recovery_ap.ip = String(recovery_ap["ip"] | config.recovery_ap.ip);
  config.recovery_ap.auto_start_when_unprovisioned =
      recovery_ap["auto_start_when_unprovisioned"] | config.recovery_ap.auto_start_when_unprovisioned;
  config.recovery_ap.start_after_wifi_failure_s =
      recovery_ap["start_after_wifi_failure_s"] | config.recovery_ap.start_after_wifi_failure_s;

  JsonObjectConst mqtt = root["mqtt"].as<JsonObjectConst>();
  config.mqtt.host = String(mqtt["host"] | "");
  config.mqtt.port = mqtt["port"] | config.mqtt.port;
  config.mqtt.client_id = String(mqtt["client_id"] | config.mqtt.client_id);
  config.mqtt.username = String(mqtt["username"] | "");
  config.mqtt.password = String(mqtt["password"] | "");
  config.mqtt.topic_prefix = String(mqtt["topic_prefix"] | config.mqtt.topic_prefix);
  config.mqtt.tls = mqtt["tls"] | false;
  config.mqtt.keepalive_s = mqtt["keepalive_s"] | config.mqtt.keepalive_s;

  JsonObjectConst heartbeat = root["heartbeat"].as<JsonObjectConst>();
  config.heartbeat.interval_s = heartbeat["interval_s"] | config.heartbeat.interval_s;
  config.heartbeat.stale_after_missed = heartbeat["stale_after_missed"] | config.heartbeat.stale_after_missed;
  config.heartbeat.offline_after_missed = heartbeat["offline_after_missed"] | config.heartbeat.offline_after_missed;

  JsonObjectConst outputs = root["outputs"].as<JsonObjectConst>();
  parseOutputJson(outputs["heating"], config.heating);
  parseOutputJson(outputs["cooling"], config.cooling);

  JsonObjectConst sensors = root["sensors"].as<JsonObjectConst>();
  config.sensors.onewire_gpio = sensors["onewire_gpio"] | config.sensors.onewire_gpio;
  config.sensors.poll_interval_s = sensors["poll_interval_s"] | config.sensors.poll_interval_s;
  config.sensors.beer_probe_rom = String(sensors["beer_probe_rom"] | "");
  config.sensors.chamber_probe_rom = String(sensors["chamber_probe_rom"] | "");

  JsonObjectConst ota = root["ota"].as<JsonObjectConst>();
  config.ota.enabled = ota["enabled"] | config.ota.enabled;
  config.ota.channel = String(ota["channel"] | config.ota.channel);
  config.ota.check_strategy = String(ota["check_strategy"] | config.ota.check_strategy);
  config.ota.check_interval_s = ota["check_interval_s"] | config.ota.check_interval_s;
  config.ota.manifest_url = String(ota["manifest_url"] | config.ota.manifest_url);
  config.ota.ca_cert_fingerprint = String(ota["ca_cert_fingerprint"] | "");
  config.ota.allow_http = ota["allow_http"] | config.ota.allow_http;

  return validateSystemConfig(config, error);
}

bool parseSystemConfigPatchJson(const String &payload, const SystemConfig &base, SystemConfig &updated, String &error) {
  DynamicJsonDocument doc(2048);
  DeserializationError parse_error = deserializeJson(doc, payload);
  if (parse_error) {
    error = "system_config patch JSON parse failed";
    return false;
  }

  updated = base;
  JsonObjectConst root = doc.as<JsonObjectConst>();

  if (root["device_id"].is<const char *>()) {
    updated.device_id = String(root["device_id"].as<const char *>());
  }
  if (root["heartbeat_interval_s"].is<uint32_t>()) {
    updated.heartbeat.interval_s = root["heartbeat_interval_s"].as<uint32_t>();
  }
  if (root["heating"].is<JsonObjectConst>()) {
    parseOutputJson(root["heating"], updated.heating);
  }
  if (root["cooling"].is<JsonObjectConst>()) {
    parseOutputJson(root["cooling"], updated.cooling);
  }
  if (root["ota"].is<JsonObjectConst>()) {
    JsonObjectConst ota = root["ota"].as<JsonObjectConst>();
    updated.ota.enabled = ota["enabled"] | updated.ota.enabled;
    updated.ota.channel = String(ota["channel"] | updated.ota.channel);
    updated.ota.check_strategy = String(ota["check_strategy"] | updated.ota.check_strategy);
    updated.ota.check_interval_s = ota["check_interval_s"] | updated.ota.check_interval_s;
    updated.ota.manifest_url = String(ota["manifest_url"] | updated.ota.manifest_url);
    updated.ota.ca_cert_fingerprint = String(ota["ca_cert_fingerprint"] | updated.ota.ca_cert_fingerprint);
    updated.ota.allow_http = ota["allow_http"] | updated.ota.allow_http;
  }

  return validateSystemConfig(updated, error);
}

bool validateSystemConfig(SystemConfig &config, String &error) {
  config.schema_version = 1;
  if (config.device_id.isEmpty()) {
    error = "device_id is required";
    return false;
  }
  if (config.wifi.ssid.isEmpty()) {
    error = "wifi ssid is required";
    return false;
  }
  if (config.wifi.password.length() < 8) {
    error = "wifi password must be at least 8 characters";
    return false;
  }
  if (config.mqtt.host.isEmpty()) {
    error = "mqtt host is required";
    return false;
  }
  if (config.mqtt.tls) {
    error = "mqtt tls is not supported in firmware_v2 yet";
    return false;
  }
  if (config.mqtt.client_id.isEmpty()) {
    config.mqtt.client_id = config.device_id;
  }
  if (config.mqtt.topic_prefix.isEmpty()) {
    error = "mqtt topic_prefix is required";
    return false;
  }
  if (config.heartbeat.interval_s < 15) {
    config.heartbeat.interval_s = 15;
  }
  if (config.sensors.onewire_gpio < 0 || config.sensors.onewire_gpio > 39) {
    error = "sensors.onewire_gpio must be between 0 and 39";
    return false;
  }
  if (config.sensors.poll_interval_s == 0) {
    config.sensors.poll_interval_s = 5;
  }
  if (config.recovery_ap.ssid.isEmpty()) {
    config.recovery_ap.ssid = "brewesp-setup-" + chipSuffix().substring(0, 6);
  }
  if (config.recovery_ap.password.length() < 8) {
    config.recovery_ap.password = "brewesp123";
  }
  if (config.ota.enabled && config.ota.manifest_url.isEmpty()) {
    error = "ota manifest_url is required when ota is enabled";
    return false;
  }
  if (!validateOutput(config.heating, "heating", error)) {
    return false;
  }
  if (!validateOutput(config.cooling, "cooling", error)) {
    return false;
  }
  config.valid = true;
  return true;
}

String serializeSystemConfig(const SystemConfig &config) {
  DynamicJsonDocument doc(4096);
  doc["schema_version"] = 1;
  doc["device_id"] = config.device_id;
  doc["timezone"] = config.timezone;

  JsonObject network = doc.createNestedObject("network");
  JsonObject wifi = network.createNestedObject("wifi");
  wifi["ssid"] = config.wifi.ssid;
  wifi["password"] = config.wifi.password;
  wifi["dhcp"] = config.wifi.dhcp;
  if (!config.wifi.static_ip.isEmpty()) {
    wifi["static_ip"] = config.wifi.static_ip;
  }
  if (!config.wifi.gateway.isEmpty()) {
    wifi["gateway"] = config.wifi.gateway;
  }
  if (!config.wifi.subnet.isEmpty()) {
    wifi["subnet"] = config.wifi.subnet;
  }
  JsonArray dns = wifi.createNestedArray("dns");
  if (!config.wifi.dns1.isEmpty()) {
    dns.add(config.wifi.dns1);
  }
  if (!config.wifi.dns2.isEmpty()) {
    dns.add(config.wifi.dns2);
  }

  JsonObject recovery_ap = network.createNestedObject("recovery_ap");
  recovery_ap["enabled"] = config.recovery_ap.enabled;
  recovery_ap["ssid"] = config.recovery_ap.ssid;
  recovery_ap["password"] = config.recovery_ap.password;
  recovery_ap["ip"] = config.recovery_ap.ip;
  recovery_ap["auto_start_when_unprovisioned"] = config.recovery_ap.auto_start_when_unprovisioned;
  recovery_ap["start_after_wifi_failure_s"] = config.recovery_ap.start_after_wifi_failure_s;

  JsonObject mqtt = doc.createNestedObject("mqtt");
  mqtt["host"] = config.mqtt.host;
  mqtt["port"] = config.mqtt.port;
  mqtt["client_id"] = config.mqtt.client_id;
  mqtt["username"] = config.mqtt.username;
  mqtt["password"] = config.mqtt.password;
  mqtt["topic_prefix"] = config.mqtt.topic_prefix;
  mqtt["tls"] = config.mqtt.tls;
  mqtt["keepalive_s"] = config.mqtt.keepalive_s;

  JsonObject heartbeat = doc.createNestedObject("heartbeat");
  heartbeat["interval_s"] = config.heartbeat.interval_s;
  heartbeat["stale_after_missed"] = config.heartbeat.stale_after_missed;
  heartbeat["offline_after_missed"] = config.heartbeat.offline_after_missed;

  JsonObject outputs = doc.createNestedObject("outputs");
  writeOutputJson(outputs.createNestedObject("heating"), config.heating);
  writeOutputJson(outputs.createNestedObject("cooling"), config.cooling);

  JsonObject sensors = doc.createNestedObject("sensors");
  sensors["onewire_gpio"] = config.sensors.onewire_gpio;
  sensors["poll_interval_s"] = config.sensors.poll_interval_s;
  if (!config.sensors.beer_probe_rom.isEmpty()) {
    sensors["beer_probe_rom"] = config.sensors.beer_probe_rom;
  }
  if (!config.sensors.chamber_probe_rom.isEmpty()) {
    sensors["chamber_probe_rom"] = config.sensors.chamber_probe_rom;
  }

  JsonObject ota = doc.createNestedObject("ota");
  ota["enabled"] = config.ota.enabled;
  ota["channel"] = config.ota.channel;
  ota["check_strategy"] = config.ota.check_strategy;
  ota["check_interval_s"] = config.ota.check_interval_s;
  ota["manifest_url"] = config.ota.manifest_url;
  ota["ca_cert_fingerprint"] = config.ota.ca_cert_fingerprint;
  ota["allow_http"] = config.ota.allow_http;

  String output;
  serializeJson(doc, output);
  return output;
}

bool parseFermentationConfigJson(const String &payload, FermentationConfig &config, String &error) {
  DynamicJsonDocument doc(4096);
  DeserializationError parse_error = deserializeJson(doc, payload);
  if (parse_error) {
    error = "fermentation_config JSON parse failed";
    return false;
  }

  config = FermentationConfig();
  JsonObjectConst root = doc.as<JsonObjectConst>();
  config.schema_version = root["schema_version"] | 2;
  config.version = root["version"] | 0;
  config.device_id = String(root["device_id"] | "");
  config.name = String(root["name"] | "");
  config.mode = String(root["mode"] | "thermostat");

  JsonObjectConst thermostat = root["thermostat"].as<JsonObjectConst>();
  config.thermostat.setpoint_c = thermostat["setpoint_c"] | config.thermostat.setpoint_c;
  config.thermostat.hysteresis_c = thermostat["hysteresis_c"] | config.thermostat.hysteresis_c;
  config.thermostat.cooling_delay_s = thermostat["cooling_delay_s"] | config.thermostat.cooling_delay_s;
  config.thermostat.heating_delay_s = thermostat["heating_delay_s"] | config.thermostat.heating_delay_s;

  JsonObjectConst sensors = root["sensors"].as<JsonObjectConst>();
  config.sensors.primary_offset_c = sensors["primary_offset_c"] | config.sensors.primary_offset_c;
  config.sensors.secondary_enabled = sensors["secondary_enabled"] | config.sensors.secondary_enabled;
  config.sensors.secondary_offset_c = sensors["secondary_offset_c"] | config.sensors.secondary_offset_c;
  config.sensors.secondary_limit_hysteresis_c =
      sensors["secondary_limit_hysteresis_c"] | config.sensors.secondary_limit_hysteresis_c;
  config.sensors.control_sensor = String(sensors["control_sensor"] | config.sensors.control_sensor);

  JsonObjectConst alarms = root["alarms"].as<JsonObjectConst>();
  config.alarms.deviation_c = alarms["deviation_c"] | config.alarms.deviation_c;
  config.alarms.sensor_stale_s = alarms["sensor_stale_s"] | config.alarms.sensor_stale_s;

  JsonObjectConst manual = root["manual"].as<JsonObjectConst>();
  config.manual.output = String(manual["output"] | config.manual.output);

  JsonObjectConst profile = root["profile"].as<JsonObjectConst>();
  config.profile.id = String(profile["id"] | config.profile.id);
  config.profile.step_count = 0;
  JsonArrayConst steps = profile["steps"].as<JsonArrayConst>();
  for (JsonObjectConst step : steps) {
    if (config.profile.step_count >= kMaxProfileSteps) {
      break;
    }
    ProfileStepConfig &entry = config.profile.steps[config.profile.step_count++];
    entry.id = String(step["id"] | entry.id);
    entry.label = String(step["label"] | entry.label);
    entry.target_c = step["target_c"] | entry.target_c;
    entry.hold_duration_s = step["hold_duration_s"] | entry.hold_duration_s;
    entry.ramp_duration_s = step["ramp_duration_s"] | entry.ramp_duration_s;
    entry.advance_policy = String(step["advance_policy"] | entry.advance_policy);
  }

  return true;
}

bool validateFermentationConfig(FermentationConfig &config, const String &expected_device_id, String &error) {
  config.schema_version = 2;
  if (config.version < 1) {
    error = "fermentation_config version must be >= 1";
    return false;
  }
  if (config.device_id.isEmpty()) {
    error = "fermentation_config device_id is required";
    return false;
  }
  if (!expected_device_id.isEmpty() && config.device_id != expected_device_id) {
    error = "fermentation_config device_id does not match this device";
    return false;
  }
  if (config.mode != "thermostat" && config.mode != "profile" && config.mode != "manual") {
    error = "fermentation_config mode must be thermostat, profile, or manual";
    return false;
  }
  if (config.thermostat.hysteresis_c < 0.1f || config.thermostat.hysteresis_c > 5.0f) {
    error = "thermostat hysteresis_c must be between 0.1 and 5.0";
    return false;
  }
  if (config.sensors.control_sensor != "primary" && config.sensors.control_sensor != "secondary") {
    error = "sensors.control_sensor must be primary or secondary";
    return false;
  }
  if (config.sensors.control_sensor == "secondary" && !config.sensors.secondary_enabled) {
    error = "secondary control requires secondary_enabled = true";
    return false;
  }
  if (config.alarms.sensor_stale_s < 5) {
    config.alarms.sensor_stale_s = 5;
  }
  if (config.mode == "manual") {
    if (config.manual.output != "off" && config.manual.output != "heating" && config.manual.output != "cooling") {
      error = "manual.output must be off, heating, or cooling";
      return false;
    }
  } else {
    config.manual.output = "off";
  }
  if (config.mode == "profile") {
    if (config.profile.id.isEmpty()) {
      error = "profile.id is required when mode is profile";
      return false;
    }
    if (config.profile.step_count == 0 || config.profile.step_count > kMaxProfileSteps) {
      error = "profile.steps must include 1-10 steps";
      return false;
    }
    for (uint8_t index = 0; index < config.profile.step_count; ++index) {
      const ProfileStepConfig &step = config.profile.steps[index];
      if (step.id.isEmpty()) {
        error = "profile.steps[].id is required";
        return false;
      }
      if (step.target_c < -20.0f || step.target_c > 50.0f) {
        error = "profile.steps[].target_c must be between -20 and 50";
        return false;
      }
      if (step.hold_duration_s > 3596400UL || step.ramp_duration_s > 3596400UL) {
        error = "profile step durations must be <= 3596400 seconds";
        return false;
      }
      if (step.advance_policy != "auto" && step.advance_policy != "manual_release") {
        error = "profile.steps[].advance_policy must be auto or manual_release";
        return false;
      }
    }
  } else {
    config.profile = ProfileConfig();
  }
  config.valid = true;
  return true;
}

String serializeFermentationConfig(const FermentationConfig &config) {
  DynamicJsonDocument doc(2048);
  doc["schema_version"] = 2;
  doc["version"] = config.version;
  doc["device_id"] = config.device_id;
  doc["name"] = config.name;
  doc["mode"] = config.mode;

  JsonObject thermostat = doc.createNestedObject("thermostat");
  thermostat["setpoint_c"] = config.thermostat.setpoint_c;
  thermostat["hysteresis_c"] = config.thermostat.hysteresis_c;
  thermostat["cooling_delay_s"] = config.thermostat.cooling_delay_s;
  thermostat["heating_delay_s"] = config.thermostat.heating_delay_s;

  JsonObject sensors = doc.createNestedObject("sensors");
  sensors["primary_offset_c"] = config.sensors.primary_offset_c;
  sensors["secondary_enabled"] = config.sensors.secondary_enabled;
  sensors["secondary_offset_c"] = config.sensors.secondary_offset_c;
  sensors["secondary_limit_hysteresis_c"] = config.sensors.secondary_limit_hysteresis_c;
  sensors["control_sensor"] = config.sensors.control_sensor;

  JsonObject alarms = doc.createNestedObject("alarms");
  alarms["deviation_c"] = config.alarms.deviation_c;
  alarms["sensor_stale_s"] = config.alarms.sensor_stale_s;

  if (config.mode == "manual") {
    JsonObject manual = doc.createNestedObject("manual");
    manual["output"] = config.manual.output;
  }

  if (config.mode == "profile") {
    JsonObject profile = doc.createNestedObject("profile");
    profile["id"] = config.profile.id;
    JsonArray steps = profile.createNestedArray("steps");
    for (uint8_t index = 0; index < config.profile.step_count; ++index) {
      const ProfileStepConfig &step = config.profile.steps[index];
      JsonObject entry = steps.createNestedObject();
      entry["id"] = step.id;
      if (!step.label.isEmpty()) {
        entry["label"] = step.label;
      }
      entry["target_c"] = step.target_c;
      entry["hold_duration_s"] = step.hold_duration_s;
      if (step.ramp_duration_s > 0) {
        entry["ramp_duration_s"] = step.ramp_duration_s;
      }
      entry["advance_policy"] = step.advance_policy;
    }
  }

  String output;
  serializeJson(doc, output);
  return output;
}

bool ConfigStore::loadProfileRuntime(uint32_t expected_config_version, ProfileRuntimeState &runtime) {
  if (!begin()) {
    return false;
  }
  const String payload = g_preferences.getString("profile_rt", "");
  if (payload.isEmpty()) {
    return false;
  }

  DynamicJsonDocument doc(1024);
  if (deserializeJson(doc, payload) != DeserializationError::Ok) {
    return false;
  }

  if ((doc["config_version"] | 0U) != expected_config_version) {
    return false;
  }

  ProfileRuntimeState loaded;
  loaded.active = doc["active"] | false;
  loaded.active_profile_id = String(doc["active_profile_id"] | "");
  loaded.active_step_id = String(doc["active_step_id"] | "");
  loaded.active_step_index = doc["active_step_index"] | -1;
  loaded.phase = String(doc["phase"] | "idle");
  loaded.paused = doc["paused"] | false;
  loaded.waiting_for_manual_release = doc["waiting_for_manual_release"] | false;
  loaded.hold_timing_active = doc["hold_timing_active"] | false;
  loaded.effective_target_c = doc["effective_target_c"] | 0.0f;
  loaded.step_base_elapsed_s = doc["step_elapsed_s"] | 0UL;
  loaded.hold_base_elapsed_s = doc["hold_elapsed_s"] | 0UL;
  runtime = loaded;
  return loaded.active;
}

bool ConfigStore::saveProfileRuntime(uint32_t config_version, const ProfileRuntimeState &runtime, uint32_t now_ms) {
  if (!begin()) {
    return false;
  }

  DynamicJsonDocument doc(1024);
  doc["config_version"] = config_version;
  doc["active"] = runtime.active;
  doc["active_profile_id"] = runtime.active_profile_id;
  doc["active_step_id"] = runtime.active_step_id;
  doc["active_step_index"] = runtime.active_step_index;
  doc["phase"] = runtime.phase;
  doc["paused"] = runtime.paused;
  doc["waiting_for_manual_release"] = runtime.waiting_for_manual_release;
  doc["hold_timing_active"] = runtime.hold_timing_active;
  doc["effective_target_c"] = runtime.effective_target_c;

  const bool timers_running = !runtime.paused && runtime.phase != "faulted" && runtime.phase != "completed";
  uint32_t step_elapsed_s = runtime.step_base_elapsed_s;
  if (timers_running && runtime.step_started_ms != 0 && static_cast<int32_t>(now_ms - runtime.step_started_ms) >= 0) {
    step_elapsed_s += (now_ms - runtime.step_started_ms) / 1000UL;
  }
  doc["step_elapsed_s"] = step_elapsed_s;

  uint32_t hold_elapsed_s = runtime.hold_base_elapsed_s;
  if (timers_running && runtime.step_hold_started_ms != 0 &&
      static_cast<int32_t>(now_ms - runtime.step_hold_started_ms) >= 0) {
    hold_elapsed_s += (now_ms - runtime.step_hold_started_ms) / 1000UL;
  }
  doc["hold_elapsed_s"] = hold_elapsed_s;

  String payload;
  const size_t serialized_length = serializeJson(doc, payload);
  if (serialized_length == 0 || serialized_length > kMaxStoredProfileRuntimeBytes) {
    return false;
  }
  return g_preferences.putString("profile_rt", payload) > 0;
}

bool ConfigStore::clearProfileRuntime() {
  if (!begin()) {
    return false;
  }
  return g_preferences.remove("profile_rt");
}
