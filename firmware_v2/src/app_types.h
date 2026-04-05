#pragma once

#include <Arduino.h>

struct RecoveryApConfig {
  bool enabled = true;
  String ssid;
  String password = "brewesp123";
  String ip = "192.168.4.1";
  bool auto_start_when_unprovisioned = true;
  uint32_t start_after_wifi_failure_s = 180;
};

struct NetworkWifiConfig {
  String ssid;
  String password;
  bool dhcp = true;
  String static_ip;
  String gateway;
  String subnet;
  String dns1;
  String dns2;
};

struct MqttConfig {
  String host;
  uint16_t port = 1883;
  String client_id;
  String username;
  String password;
  String topic_prefix = "brewesp";
  bool tls = false;
  uint16_t keepalive_s = 60;
};

struct HeartbeatConfig {
  uint32_t interval_s = 60;
  uint8_t stale_after_missed = 2;
  uint8_t offline_after_missed = 3;
};

struct OutputConfig {
  String driver = "gpio";
  int pin = -1;
  String active_level = "high";
  String host;
  uint16_t port = 0;
  uint8_t switch_id = 0;
  String username;
  String password;
  bool https = false;
  String alias;
  uint32_t poll_interval_s = 30;
};

struct SensorsConfig {
  int onewire_gpio = 32;
  uint32_t poll_interval_s = 5;
  String beer_probe_rom;
  String chamber_probe_rom;
};

struct OtaConfig {
  bool enabled = true;
  String channel = "stable";
  String check_strategy = "manual";
  uint32_t check_interval_s = 86400;
  String manifest_url = "http://web.local/firmware/manifest/stable.json";
  String ca_cert_fingerprint;
  bool allow_http = false;
};

constexpr char kConfigOwnerLocal[] = "local";
constexpr char kConfigOwnerExternal[] = "external";

inline bool isValidConfigOwner(const String &owner) {
  return owner == kConfigOwnerLocal || owner == kConfigOwnerExternal;
}

inline bool isExternalConfigOwner(const String &owner) {
  return owner == kConfigOwnerExternal;
}

struct SystemConfig {
  int schema_version = 1;
  String device_id;
  String timezone = "Europe/Stockholm";
  String config_owner = kConfigOwnerExternal;
  NetworkWifiConfig wifi;
  RecoveryApConfig recovery_ap;
  MqttConfig mqtt;
  HeartbeatConfig heartbeat;
  OutputConfig heating;
  OutputConfig cooling;
  SensorsConfig sensors;
  OtaConfig ota;
  bool valid = false;
};

struct ThermostatConfig {
  float setpoint_c = 20.0f;
  float hysteresis_c = 0.3f;
  uint32_t cooling_delay_s = 300;
  uint32_t heating_delay_s = 120;
};

struct SensorControlConfig {
  float primary_offset_c = 0.0f;
  bool secondary_enabled = false;
  float secondary_offset_c = 0.0f;
  float secondary_limit_hysteresis_c = 1.5f;
  String control_sensor = "primary";
};

struct AlarmConfig {
  float deviation_c = 2.0f;
  uint32_t sensor_stale_s = 30;
};

constexpr uint8_t kMaxProfileSteps = 10;

struct ManualModeConfig {
  String output = "off";
};

struct ProfileStepConfig {
  String id;
  String label;
  float target_c = 20.0f;
  uint32_t hold_duration_s = 0;
  uint32_t ramp_duration_s = 0;
  String advance_policy = "auto";
};

struct ProfileConfig {
  String id;
  uint8_t step_count = 0;
  ProfileStepConfig steps[kMaxProfileSteps];
};

struct ProfileRuntimeState {
  bool active = false;
  String active_profile_id;
  String active_step_id;
  int active_step_index = -1;
  String phase = "idle";
  bool paused = false;
  bool waiting_for_manual_release = false;
  bool hold_timing_active = false;
  float effective_target_c = 0.0f;
  uint32_t step_started_ms = 0;
  uint32_t step_hold_started_ms = 0;
  uint32_t step_base_elapsed_s = 0;
  uint32_t hold_base_elapsed_s = 0;
};

struct FermentationConfig {
  int schema_version = 2;
  uint32_t version = 0;
  String device_id;
  String name;
  String mode = "thermostat";
  ThermostatConfig thermostat;
  SensorControlConfig sensors;
  AlarmConfig alarms;
  ManualModeConfig manual;
  ProfileConfig profile;
  bool valid = false;
};

struct ProbeReading {
  bool present = false;
  bool valid = false;
  bool stale = true;
  String rom;
  float raw_c = NAN;
  float adjusted_c = NAN;
  uint32_t last_seen_ms = 0;
};

struct SensorSnapshot {
  ProbeReading beer;
  ProbeReading chamber;
};

struct DriverStatus {
  bool known = false;
  bool on = false;
  String description;
};

inline String outputStateString(const DriverStatus &status) {
  if (!status.known) {
    return "unknown";
  }
  return status.on ? "on" : "off";
}
