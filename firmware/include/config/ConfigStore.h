#pragma once

#include "config/FermentationConfig.h"
#include "config/SystemConfig.h"

class ConfigStore {
public:
    bool load(SystemConfig& config);
    bool save(const SystemConfig& config);
    bool loadFermentationConfig(FermentationConfig& config);
    bool saveFermentationConfig(const FermentationConfig& config);
};
