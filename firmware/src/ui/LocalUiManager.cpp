#include "ui/LocalUiManager.h"

#include "support/Logger.h"

bool LocalUiManager::begin(const SystemConfig& config) {
    enabled_ = config.localUi.enabled;
    headless_ = !enabled_;

    if (enabled_) {
        LOG_INFO_MSG("[ui] local UI enabled");
        LOG_DEBUG(
            "[ui] display=%s address=%s buttons=%s\r\n",
            config.display.driver.c_str(),
            config.display.i2cAddress.c_str(),
            config.buttons.enabled ? "enabled" : "disabled");
    } else {
        LOG_INFO_MSG("[ui] headless mode");
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
