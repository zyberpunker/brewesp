#pragma once

#include "config/FermentationConfig.h"
#include "config/SystemConfig.h"

class ConfigStore {
public:
    bool load(SystemConfig& config);
    bool save(const SystemConfig& config);
    bool loadFermentationConfig(FermentationConfig& config);
    bool saveFermentationConfig(const FermentationConfig& config);
    bool loadProfileRuntime(uint32_t expectedConfigVersion, ProfileRuntimeState& runtime);
    bool saveProfileRuntime(uint32_t configVersion, const ProfileRuntimeState& runtime, uint32_t nowMs);
    bool clearProfileRuntime();
};
