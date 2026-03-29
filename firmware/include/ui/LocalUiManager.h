#pragma once

#include "config/SystemConfig.h"

class LocalUiManager {
public:
    bool begin(const SystemConfig& config);
    void update();

    bool isEnabled() const;
    bool isHeadless() const;

private:
    bool enabled_ = false;
    bool headless_ = true;
};
