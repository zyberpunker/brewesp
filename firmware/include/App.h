#pragma once

#include "controller/ControllerEngine.h"
#include "config/ConfigStore.h"
#include "config/FermentationConfig.h"
#include "config/SystemConfig.h"
#include "network/MqttManager.h"
#include "network/ProvisioningManager.h"
#include "output/KasaDiscovery.h"
#include "output/OutputManager.h"
#include "ui/LocalUiManager.h"

class App {
public:
    void begin();
    void update();

private:
    MqttManager::TelemetrySnapshot buildTelemetrySnapshot() const;
    FermentationConfig buildDefaultFermentationConfig(const String& deviceId) const;
    void beginNormalMode();
    void startProvisioningMode(const char* reason);
    void ensureWifiConnected();
    void handleSystemConfig(const SystemConfig& updatedConfig);
    void handleFermentationConfig(const FermentationConfig& updatedConfig);
    void handleOutputCommand(const String& target, OutputState state);
    void runKasaDiscovery();
    SystemConfig buildDefaultConfig() const;
    ControllerEngine::Inputs buildControllerInputs() const;

    SystemConfig config_;
    FermentationConfig fermentationConfig_;
    ControllerEngine controller_;
    ConfigStore configStore_;
    ProvisioningManager provisioning_;
    MqttManager mqtt_;
    KasaDiscovery kasaDiscovery_;
    OutputManager outputs_;
    LocalUiManager localUi_;
    uint32_t lastHeartbeatLogMs_ = 0;
    uint32_t wifiConnectStartedMs_ = 0;
    bool provisioningMode_ = false;
};
