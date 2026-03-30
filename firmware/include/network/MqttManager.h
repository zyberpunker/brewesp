#pragma once

#include <functional>

#include <PubSubClient.h>
#include <WiFiClient.h>

#include "config/FermentationConfig.h"
#include "config/SystemConfig.h"
#include "output/OutputDriver.h"

class LocalUiManager;
class OutputManager;

class MqttManager {
public:
    struct TelemetrySnapshot {
        bool hasPrimaryTemp = false;
        float primaryTempC = 0.0f;
        bool hasSecondaryTemp = false;
        float secondaryTempC = 0.0f;
        bool hasSetpoint = false;
        float setpointC = 0.0f;
        float hysteresisC = 0.3f;
        uint16_t coolingDelaySeconds = 300;
        uint16_t heatingDelaySeconds = 120;
        String mode = "thermostat";
        String controllerState = "idle";
        String controllerReason = "idle";
        bool automaticControlActive = false;
        bool secondarySensorEnabled = false;
        String controlSensor = "primary";
        bool beerProbePresent = false;
        bool beerProbeValid = false;
        String beerProbeRom;
        bool chamberProbePresent = false;
        bool chamberProbeValid = false;
        String chamberProbeRom;
        String profileId;
        String profileStepId;
        int profileStepIndex = -1;
        bool hasEffectiveTarget = false;
        float effectiveTargetC = 0.0f;
        bool hasProfileRuntime = false;
        String profilePhase = "idle";
        bool profilePaused = false;
        bool waitingForManualRelease = false;
        uint32_t activeConfigVersion = 0;
        uint32_t stepStartedAtSeconds = 0;
        uint32_t stepHoldStartedAtSeconds = 0;
    };

    using SystemConfigHandler = std::function<void(const SystemConfig&)>;
    using FermentationConfigHandler = std::function<void(const FermentationConfig&)>;
    using OutputCommandHandler = std::function<void(const String&, OutputState)>;
    using ProfileCommandHandler = std::function<void(const String&, const String&)>;
    using DiscoveryRequestHandler = std::function<void()>;

    bool begin(const SystemConfig& config);
    void update(
        const SystemConfig& config,
        const OutputManager& outputs,
        const LocalUiManager& localUi,
        const TelemetrySnapshot& telemetry);
    bool isConnected();
    void setSystemConfigHandler(SystemConfigHandler handler);
    void setFermentationConfigHandler(FermentationConfigHandler handler);
    void setOutputCommandHandler(OutputCommandHandler handler);
    void setProfileCommandHandler(ProfileCommandHandler handler);
    void setDiscoveryRequestHandler(DiscoveryRequestHandler handler);
    void setAppliedFermentationVersion(uint32_t version);
    void publishState(
        const SystemConfig& config,
        const OutputManager& outputs,
        const LocalUiManager& localUi,
        const TelemetrySnapshot& telemetry);
    void publishConfigApplied(
        const SystemConfig& config,
        uint32_t requestedVersion,
        uint32_t appliedVersion,
        const char* result,
        const char* message);
    void publishKasaDiscovery(const SystemConfig& config, const String& devicePayload);

private:
    struct PendingConfigApplied {
        bool active = false;
        uint32_t requestedVersion = 0;
        uint32_t appliedVersion = 0;
        String result;
        String message;
    };

    bool connectIfNeeded(
        const SystemConfig& config,
        const OutputManager& outputs,
        const LocalUiManager& localUi,
        const TelemetrySnapshot& telemetry);
    void flushPendingFermentationWork();
    void handleMessage(const SystemConfig& config, char* topic, uint8_t* payload, unsigned int length);
    void handleSystemConfig(const SystemConfig& currentConfig, const String& payload);
    void handleFermentationConfig(const String& payload);
    void handleCommand(const String& payload);
    void publishAvailability(const SystemConfig& config, const char* status);
    void publishHeartbeat(
        const SystemConfig& config,
        const OutputManager& outputs,
        const LocalUiManager& localUi);
    void publishTelemetry(
        const SystemConfig& config,
        const OutputManager& outputs,
        const TelemetrySnapshot& telemetry);
    String topicFor(const SystemConfig& config, const char* suffix) const;

    WiFiClient wifiClient_;
    PubSubClient client_{wifiClient_};
    uint32_t lastReconnectAttemptMs_ = 0;
    uint32_t lastHeartbeatMs_ = 0;
    uint32_t lastTelemetryMs_ = 0;
    SystemConfig currentConfig_;
    SystemConfigHandler systemConfigHandler_;
    FermentationConfigHandler fermentationConfigHandler_;
    OutputCommandHandler outputCommandHandler_;
    ProfileCommandHandler profileCommandHandler_;
    DiscoveryRequestHandler discoveryRequestHandler_;
    uint32_t lastAppliedFermentationVersion_ = 0;
    bool hasPendingFermentationConfig_ = false;
    FermentationConfig pendingFermentationConfig_;
    PendingConfigApplied pendingConfigApplied_;
};
