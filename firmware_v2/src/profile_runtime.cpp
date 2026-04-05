#include "profile_runtime.h"

namespace {
constexpr uint32_t kProfileRuntimePersistIntervalMs = 300000UL;

uint32_t nextConfigVersion(uint32_t current) {
  return current == 0 ? 1 : current + 1;
}
}

bool ProfileRuntimeManager::syncToConfig(const FermentationConfig &config, ConfigStore &store, uint32_t now_ms) {
  reset();

  if (config.mode != "profile" || config.profile.step_count == 0) {
    clearPersistedRuntime(store);
    return false;
  }

  if (!restore(config, store, now_ms)) {
    activateStep(config, 0, true, now_ms);
  }

  const bool changed = update(config, true, store, now_ms);
  persist(store, config.version, now_ms, true);
  return changed || state_.active;
}

bool ProfileRuntimeManager::update(const FermentationConfig &config, bool allow_progress, ConfigStore &store,
                                   uint32_t now_ms) {
  if (config.mode != "profile" || config.profile.step_count == 0 || !state_.active) {
    return false;
  }

  if (!matchesConfig(config)) {
    clearPersistedRuntime(store);
    reset();
    return true;
  }

  const ProfileStepConfig &step = config.profile.steps[state_.active_step_index];
  const float previous_target =
      state_.active_step_index == 0 ? step.target_c : config.profile.steps[state_.active_step_index - 1].target_c;
  bool changed = false;

  if (state_.phase == "completed") {
    state_.effective_target_c = step.target_c;
    persist(store, config.version, now_ms, false);
    return false;
  }

  if (!allow_progress) {
    if (state_.phase != "faulted") {
      freezeTimers(now_ms);
      state_.phase = "faulted";
      changed = true;
    }
    persist(store, config.version, now_ms, changed);
    return changed;
  }

  if (state_.phase == "faulted") {
    resumeTimers(now_ms);
  }

  if (state_.paused) {
    if (state_.phase != "paused") {
      state_.phase = "paused";
      changed = true;
    }
    persist(store, config.version, now_ms, changed);
    return changed;
  }

  const uint32_t step_elapsed_s = currentStepElapsedSeconds(now_ms);
  if (state_.active_step_index > 0 && step.ramp_duration_s > 0 && step_elapsed_s < step.ramp_duration_s) {
    const float ramp_progress = static_cast<float>(step_elapsed_s) / static_cast<float>(step.ramp_duration_s);
    if (state_.phase != "ramping") {
      state_.phase = "ramping";
      changed = true;
    }
    if (state_.waiting_for_manual_release) {
      state_.waiting_for_manual_release = false;
      changed = true;
    }
    if (state_.hold_timing_active) {
      state_.hold_timing_active = false;
      changed = true;
    }
    const float next_target = previous_target + ((step.target_c - previous_target) * ramp_progress);
    state_.effective_target_c = next_target;
    if (state_.step_hold_started_ms != 0 || state_.hold_base_elapsed_s != 0) {
      state_.step_hold_started_ms = 0;
      state_.hold_base_elapsed_s = 0;
      changed = true;
    }
    persist(store, config.version, now_ms, false);
    return changed;
  }

  state_.effective_target_c = step.target_c;
  if (!state_.hold_timing_active) {
    state_.hold_timing_active = true;
    changed = true;
  }
  if (state_.step_hold_started_ms == 0) {
    state_.step_hold_started_ms = now_ms;
    state_.hold_base_elapsed_s = 0;
    changed = true;
  }

  const bool waiting_for_manual_release = step.advance_policy == "manual_release";
  const String next_phase = waiting_for_manual_release ? "waiting_manual_release" : "holding";
  if (state_.phase != next_phase) {
    state_.phase = next_phase;
    changed = true;
  }
  if (state_.waiting_for_manual_release != waiting_for_manual_release) {
    state_.waiting_for_manual_release = waiting_for_manual_release;
    changed = true;
  }

  if (!waiting_for_manual_release) {
    const uint32_t hold_elapsed_s = currentHoldElapsedSeconds(now_ms);
    if (hold_elapsed_s >= step.hold_duration_s) {
      if ((state_.active_step_index + 1) < config.profile.step_count) {
        const bool advanced = activateStep(config, static_cast<uint8_t>(state_.active_step_index + 1), true, now_ms);
        if (advanced) {
          persist(store, config.version, now_ms, true);
        }
        return advanced;
      }

      if (state_.phase != "completed") {
        state_.phase = "completed";
        state_.waiting_for_manual_release = false;
        changed = true;
      }
    }
  }

  persist(store, config.version, now_ms, changed);
  return changed;
}

bool ProfileRuntimeManager::handleCommand(FermentationConfig &config, const String &command, const String &step_id,
                                          ConfigStore &store, uint32_t now_ms) {
  if (command == "profile_stop") {
    if (config.mode != "profile") {
      return false;
    }
    const FermentationConfig previous = config;
    const float fallback_setpoint_c = effectiveTargetC(config);
    config.mode = "thermostat";
    config.version = nextConfigVersion(config.version);
    config.thermostat.setpoint_c = fallback_setpoint_c;
    if (!store.saveFermentationConfig(config)) {
      config = previous;
      return false;
    }
    clearPersistedRuntime(store);
    reset();
    return true;
  }

  if (config.mode != "profile" || config.profile.step_count == 0 || !state_.active) {
    return false;
  }

  const ProfileRuntimeState previous = state_;
  bool changed = false;

  if (command == "profile_pause") {
    if (!state_.paused && state_.phase != "completed") {
      freezeTimers(now_ms);
      state_.paused = true;
      state_.phase = "paused";
      changed = true;
    }
  } else if (command == "profile_resume") {
    if (state_.paused) {
      resumeTimers(now_ms);
      state_.paused = false;
      changed = true;
    }
  } else if (command == "profile_release_hold") {
    if (state_.waiting_for_manual_release) {
      const int next_step = state_.active_step_index + 1;
      if (next_step >= config.profile.step_count) {
        state_.waiting_for_manual_release = false;
        state_.phase = "completed";
        changed = true;
      } else {
        changed = activateStep(config, static_cast<uint8_t>(next_step), true, now_ms);
      }
    }
  } else if (command == "profile_jump_to_step") {
    for (uint8_t index = 0; index < config.profile.step_count; ++index) {
      if (config.profile.steps[index].id == step_id) {
        changed = activateStep(config, index, true, now_ms);
        break;
      }
    }
  }

  if (!changed) {
    return false;
  }

  update(config, true, store, now_ms);
  if (!persist(store, config.version, now_ms, true)) {
    state_ = previous;
    return false;
  }
  return true;
}

const ProfileRuntimeState &ProfileRuntimeManager::state() const {
  return state_;
}

bool ProfileRuntimeManager::active() const {
  return state_.active;
}

float ProfileRuntimeManager::effectiveTargetC(const FermentationConfig &config) const {
  if (config.mode == "profile" && state_.active) {
    return state_.effective_target_c;
  }
  return config.thermostat.setpoint_c;
}

float ProfileRuntimeManager::activeStepTargetC(const FermentationConfig &config) const {
  if (config.mode == "profile" && state_.active && matchesConfig(config)) {
    return config.profile.steps[state_.active_step_index].target_c;
  }
  return config.thermostat.setpoint_c;
}

uint32_t ProfileRuntimeManager::stepStartedAtSeconds(uint32_t now_ms) const {
  const uint32_t elapsed_s = currentStepElapsedSeconds(now_ms);
  const uint32_t uptime_s = now_ms / 1000UL;
  return elapsed_s > uptime_s ? 0 : uptime_s - elapsed_s;
}

bool ProfileRuntimeManager::hasStepHoldStarted() const {
  return state_.step_hold_started_ms != 0 || state_.hold_base_elapsed_s != 0;
}

uint32_t ProfileRuntimeManager::stepHoldStartedAtSeconds(uint32_t now_ms) const {
  const uint32_t elapsed_s = currentHoldElapsedSeconds(now_ms);
  const uint32_t uptime_s = now_ms / 1000UL;
  return elapsed_s > uptime_s ? 0 : uptime_s - elapsed_s;
}

bool ProfileRuntimeManager::activateStep(const FermentationConfig &config, uint8_t step_index, bool fresh_step,
                                         uint32_t now_ms) {
  if (step_index >= config.profile.step_count) {
    return false;
  }

  const ProfileStepConfig &step = config.profile.steps[step_index];
  const float previous_target = step_index == 0 ? step.target_c : config.profile.steps[step_index - 1].target_c;
  const bool has_ramp = step_index > 0 && step.ramp_duration_s > 0;

  state_.active = true;
  state_.active_profile_id = config.profile.id;
  state_.active_step_id = step.id;
  state_.active_step_index = step_index;
  state_.paused = false;
  state_.waiting_for_manual_release = false;
  state_.hold_timing_active = !has_ramp;
  state_.phase = has_ramp ? "ramping" : (step.advance_policy == "manual_release" ? "waiting_manual_release" : "holding");
  state_.effective_target_c = has_ramp ? previous_target : step.target_c;

  if (fresh_step) {
    state_.step_started_ms = now_ms;
    state_.step_hold_started_ms = has_ramp ? 0 : now_ms;
    state_.step_base_elapsed_s = 0;
    state_.hold_base_elapsed_s = 0;
  }

  return true;
}

bool ProfileRuntimeManager::restore(const FermentationConfig &config, ConfigStore &store, uint32_t now_ms) {
  ProfileRuntimeState restored;
  if (!store.loadProfileRuntime(config.version, restored)) {
    return false;
  }

  state_ = restored;
  if (!matchesConfig(config)) {
    reset();
    clearPersistedRuntime(store);
    return false;
  }

  state_.step_started_ms = now_ms;
  state_.step_hold_started_ms = state_.hold_timing_active ? now_ms : 0;
  return true;
}

bool ProfileRuntimeManager::persist(ConfigStore &store, uint32_t config_version, uint32_t now_ms, bool force) {
  if (!state_.active) {
    return false;
  }
  if (!force && (now_ms - last_persist_ms_) < kProfileRuntimePersistIntervalMs) {
    return false;
  }
  if (!store.saveProfileRuntime(config_version, state_, now_ms)) {
    return false;
  }
  last_persist_ms_ = now_ms;
  return true;
}

void ProfileRuntimeManager::reset() {
  state_ = ProfileRuntimeState();
  last_persist_ms_ = 0;
}

void ProfileRuntimeManager::clearPersistedRuntime(ConfigStore &store) {
  store.clearProfileRuntime();
  last_persist_ms_ = 0;
}

uint32_t ProfileRuntimeManager::currentStepElapsedSeconds(uint32_t now_ms) const {
  uint32_t elapsed_s = state_.step_base_elapsed_s;
  if (!state_.paused && state_.phase != "faulted" && state_.phase != "completed" && state_.step_started_ms != 0 &&
      static_cast<int32_t>(now_ms - state_.step_started_ms) >= 0) {
    elapsed_s += (now_ms - state_.step_started_ms) / 1000UL;
  }
  return elapsed_s;
}

uint32_t ProfileRuntimeManager::currentHoldElapsedSeconds(uint32_t now_ms) const {
  uint32_t elapsed_s = state_.hold_base_elapsed_s;
  if (!state_.paused && state_.phase != "faulted" && state_.phase != "completed" && state_.step_hold_started_ms != 0 &&
      static_cast<int32_t>(now_ms - state_.step_hold_started_ms) >= 0) {
    elapsed_s += (now_ms - state_.step_hold_started_ms) / 1000UL;
  }
  return elapsed_s;
}

void ProfileRuntimeManager::freezeTimers(uint32_t now_ms) {
  state_.step_base_elapsed_s = currentStepElapsedSeconds(now_ms);
  state_.step_started_ms = now_ms;
  state_.hold_base_elapsed_s = currentHoldElapsedSeconds(now_ms);
  if (state_.step_hold_started_ms != 0) {
    state_.step_hold_started_ms = now_ms;
  }
}

void ProfileRuntimeManager::resumeTimers(uint32_t now_ms) {
  state_.step_started_ms = now_ms;
  if (state_.step_hold_started_ms != 0 || state_.hold_base_elapsed_s > 0) {
    state_.step_hold_started_ms = now_ms;
  }
}

bool ProfileRuntimeManager::matchesConfig(const FermentationConfig &config) const {
  return state_.active && state_.active_profile_id == config.profile.id && state_.active_step_index >= 0 &&
         state_.active_step_index < config.profile.step_count &&
         config.profile.steps[state_.active_step_index].id == state_.active_step_id;
}
