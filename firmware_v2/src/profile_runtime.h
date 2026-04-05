#pragma once

#include "app_types.h"
#include "config_store.h"

class ProfileRuntimeManager {
 public:
  bool syncToConfig(const FermentationConfig &config, ConfigStore &store, uint32_t now_ms);
  bool update(const FermentationConfig &config, bool allow_progress, ConfigStore &store, uint32_t now_ms);
  bool handleCommand(FermentationConfig &config, const String &command, const String &step_id, ConfigStore &store,
                     uint32_t now_ms);

  const ProfileRuntimeState &state() const;
  bool active() const;
  float effectiveTargetC(const FermentationConfig &config) const;
  float activeStepTargetC(const FermentationConfig &config) const;
  uint32_t stepStartedAtSeconds(uint32_t now_ms) const;
  bool hasStepHoldStarted() const;
  uint32_t stepHoldStartedAtSeconds(uint32_t now_ms) const;

 private:
  bool activateStep(const FermentationConfig &config, uint8_t step_index, bool fresh_step, uint32_t now_ms);
  bool restore(const FermentationConfig &config, ConfigStore &store, uint32_t now_ms);
  bool persist(ConfigStore &store, uint32_t config_version, uint32_t now_ms, bool force);
  void reset();
  void clearPersistedRuntime(ConfigStore &store);
  uint32_t currentStepElapsedSeconds(uint32_t now_ms) const;
  uint32_t currentHoldElapsedSeconds(uint32_t now_ms) const;
  void freezeTimers(uint32_t now_ms);
  void resumeTimers(uint32_t now_ms);
  bool matchesConfig(const FermentationConfig &config) const;

  ProfileRuntimeState state_;
  uint32_t last_persist_ms_ = 0;
};
