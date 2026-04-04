#pragma once

#include "controller/ControllerEngine.h"
#include "config/ConfigStore.h"
#include "config/FermentationConfig.h"
#include "config/SystemConfig.h"
#include "network/MqttManager.h"
#include "network/ProvisioningManager.h"
#include "ota/OtaManager.h"
#include "output/KasaDiscovery.h"
#include "output/OutputManager.h"
#include "sensor/SensorManager.h"
#include "ui/LocalUiManager.h"

class App {
public:
    void begin();
    void update();

private:
    MqttManager::TelemetrySnapshot buildTelemetrySnapshot();
    FermentationConfig buildDefaultFermentationConfig(const String& deviceId) const;
    void beginNormalMode();
    void startProvisioningMode(const char* reason);
    void ensureWifiConnected();
    bool updateProfileRuntime(uint32_t nowMs, bool allowProgress);
    bool restoreProfileRuntime(uint32_t nowMs);
    bool persistProfileRuntime(uint32_t nowMs, bool force);
    void clearPersistedProfileRuntime();
    uint32_t currentProfileStepElapsedSeconds(uint32_t nowMs) const;
    uint32_t currentProfileHoldElapsedSeconds(uint32_t nowMs) const;
    void freezeProfileTimers(uint32_t nowMs);
    void resumeProfileTimers(uint32_t nowMs);
    void updateControlLoop(uint32_t nowMs);
    void handleSystemConfig(const SystemConfig& updatedConfig);
    void handleFermentationConfig(const FermentationConfig& updatedConfig);
    void handleOutputCommand(const String& target, OutputState state);
    void handleProfileCommand(const String& command, const String& stepId);
    void handleAlertAcknowledgement(const String& code);
    void resetProfileRuntime();
    void initializeProfileRuntime();
    bool activateProfileStep(uint8_t stepIndex, bool treatAsFreshStep);
    void handleOtaCommand(const String& command, const String& channel);
    void processPendingOtaCommand();
    bool isOtaLockoutActive() const;
    void runKasaDiscovery();
    SystemConfig buildDefaultConfig() const;
    ControllerEngine::Inputs buildControllerInputs() const;
    String buildAlertSignature(const MqttManager::TelemetrySnapshot& telemetry) const;
    bool acknowledgeAlertCode(const String& code);
    bool isAlertAcknowledged(const char* code) const;
    void clearAlertAcknowledgement(const char* code);

    SystemConfig config_;
    FermentationConfig fermentationConfig_;
    FermentationConfig fermentationConfigRollback_;
    ControllerEngine controller_;
    ConfigStore configStore_;
    ProvisioningManager provisioning_;
    MqttManager mqtt_;
    OtaManager ota_;
    KasaDiscovery kasaDiscovery_;
    OutputManager outputs_;
    SensorManager sensors_;
    LocalUiManager localUi_;
    ProfileRuntimeState profileRuntime_;
    ProfileRuntimeState profileRuntimeRollback_;
    uint32_t lastProfileRuntimePersistMs_ = 0;
    uint32_t lastHeartbeatLogMs_ = 0;
    uint32_t wifiConnectStartedMs_ = 0;
    uint32_t otaRestartAtMs_ = 0;
    bool provisioningMode_ = false;
    bool otaShutdownPending_ = false;
    bool ackControlSensorMissing_ = false;
    bool ackControlSensorInvalid_ = false;
    bool ackControlSensorStale_ = false;
    bool ackSecondarySensorMissing_ = false;
    bool ackSecondarySensorInvalid_ = false;
    bool ackSecondarySensorStale_ = false;
    bool ackSensorDeviationHigh_ = false;
    String lastPublishedAlertSignature_;
    String pendingOtaCommand_;
    String pendingOtaChannel_;
};
