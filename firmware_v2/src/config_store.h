#pragma once

#include <Arduino.h>

#include "app_types.h"

class ConfigStore {
 public:
  bool begin();
  bool loadSystemConfig(SystemConfig &config);
  bool saveSystemConfig(const SystemConfig &config);
  bool loadFermentationConfig(FermentationConfig &config);
  bool saveFermentationConfig(const FermentationConfig &config);
  bool loadProfileRuntime(uint32_t expected_config_version, ProfileRuntimeState &runtime);
  bool saveProfileRuntime(uint32_t config_version, const ProfileRuntimeState &runtime, uint32_t now_ms);
  bool clearProfileRuntime();

 private:
  bool started_ = false;
};

String defaultDeviceId();
SystemConfig defaultSystemConfig();
FermentationConfig defaultFermentationConfig(const String &device_id);

bool parseSystemConfigJson(const String &payload, SystemConfig &config, String &error);
bool parseSystemConfigPatchJson(const String &payload, const SystemConfig &base, SystemConfig &updated, String &error);
bool validateSystemConfig(SystemConfig &config, String &error);
String serializeSystemConfig(const SystemConfig &config);

bool parseFermentationConfigJson(const String &payload, FermentationConfig &config, String &error);
bool validateFermentationConfig(FermentationConfig &config, const String &expected_device_id, String &error);
String serializeFermentationConfig(const FermentationConfig &config);
