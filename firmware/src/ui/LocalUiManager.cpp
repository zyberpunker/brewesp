#include "ui/LocalUiManager.h"

bool LocalUiManager::begin(const SystemConfig& config) {
    enabled_ = config.localUi.enabled;
    headless_ = !enabled_;

    if (enabled_) {
        Serial.println("[ui] local UI enabled");
        Serial.printf(
            "[ui] display=%s address=%s buttons=%s\r\n",
            config.display.driver.c_str(),
            config.display.i2cAddress.c_str(),
            config.buttons.enabled ? "enabled" : "disabled");
    } else {
        Serial.println("[ui] headless mode");
    }

    return true;
}

void LocalUiManager::update() {}

bool LocalUiManager::isEnabled() const {
    return enabled_;
}

bool LocalUiManager::isHeadless() const {
    return headless_;
}
