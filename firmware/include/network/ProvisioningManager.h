#pragma once

#include <WebServer.h>

#include <functional>

#include "config/SystemConfig.h"

class ProvisioningManager {
public:
    using SaveCallback = std::function<bool(const SystemConfig&)>;

    bool begin(const SystemConfig& currentConfig, SaveCallback onSave);
    void update();
    bool isActive() const;
    bool restartRequested() const;
    const String& accessPointSsid() const;

private:
    void handleRoot();
    void handleSave();
    String buildHtmlPage() const;
    String buildRecoverySsid(const SystemConfig& config) const;

    WebServer server_{80};
    SaveCallback onSave_;
    SystemConfig currentConfig_;
    bool active_ = false;
    bool restartRequested_ = false;
    String accessPointSsid_;
};
