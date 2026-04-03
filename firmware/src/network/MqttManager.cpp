#include "network/MqttManager.h"

#include <math.h>

#include <ArduinoJson.h>
#include <WiFi.h>

#include "config/FirmwareVersion.h"
#include "output/OutputManager.h"
#include "support/Logger.h"
#include "ui/LocalUiManager.h"

namespace {
constexpr uint32_t kTelemetryIntervalMs = 15000UL;
constexpr uint16_t kMqttBufferSize = 2048U;
constexpr uint16_t kMqttKeepAliveSeconds = 60U;

const char* outputStateName(OutputState state) {
    switch (state) {
        case OutputState::On:
            return "on";
        case OutputState::Off:
            return "off";
        case OutputState::Unknown:
        default:
            return "unknown";
    }
}

OutputDriverType outputDriverFromString(const String& value) {
    if (value == "gpio") {
        return OutputDriverType::Gpio;
    }
    if (value == "shelly_http_rpc") {
        return OutputDriverType::ShellyHttpRpc;
    }
    if (value == "kasa_local") {
        return OutputDriverType::KasaLocal;
    }
    if (value == "generic_mqtt_relay") {
        return OutputDriverType::GenericMqttRelay;
    }
    return OutputDriverType::None;
}

OutputState outputStateFromString(const String& value) {
    if (value == "on") {
        return OutputState::On;
    }
    if (value == "off") {
        return OutputState::Off;
    }
    return OutputState::Unknown;
}
}

bool MqttManager::begin(const SystemConfig& config) {
    currentConfig_ = config;
    client_.setServer(config.mqtt.host.c_str(), config.mqtt.port);
    client_.setBufferSize(kMqttBufferSize);
    client_.setKeepAlive(kMqttKeepAliveSeconds);
    client_.setCallback(
        [this](char* topic, uint8_t* payload, unsigned int length) {
            handleMessage(currentConfig_, topic, payload, length);
        });
    LOG_INFO(
        "[mqtt] begin host=%s port=%u client_id=%s topic_prefix=%s device_id=%s keepalive_s=%u\r\n",
        config.mqtt.host.c_str(),
        config.mqtt.port,
        config.mqtt.clientId.c_str(),
        config.mqtt.topicPrefix.c_str(),
        config.deviceId.c_str(),
        static_cast<unsigned>(kMqttKeepAliveSeconds));
    return true;
}

void MqttManager::update(
    const SystemConfig& config,
    const OutputManager& outputs,
    const LocalUiManager& localUi,
    const TelemetrySnapshot& telemetry) {
    if (config.mqtt.host.isEmpty() || WiFi.status() != WL_CONNECTED) {
        return;
    }

    if (connectIfNeeded(config, outputs, localUi, telemetry)) {
        client_.loop();
        flushPendingWork();
        const bool connected = client_.connected();
        if (!connectionStateKnown_ || lastKnownConnectionState_ != connected) {
            connectionStateKnown_ = true;
            lastKnownConnectionState_ = connected;
            LOG_INFO("[mqtt] connection state=%s rc=%d\r\n", connected ? "connected" : "disconnected", client_.state());
        }
        return;
    }

    client_.loop();
    flushPendingWork();

    const bool connected = client_.connected();
    if (!connectionStateKnown_ || lastKnownConnectionState_ != connected) {
        connectionStateKnown_ = true;
        lastKnownConnectionState_ = connected;
        LOG_INFO("[mqtt] connection state=%s rc=%d\r\n", connected ? "connected" : "disconnected", client_.state());
    }

    if (!connected) {
        return;
    }

    const uint32_t now = millis();
    if (now - lastHeartbeatMs_ >= config.heartbeat.intervalSeconds * 1000UL) {
        lastHeartbeatMs_ = now;
        publishHeartbeat(config, outputs, localUi);
    }

    if (now - lastTelemetryMs_ >= kTelemetryIntervalMs) {
        lastTelemetryMs_ = now;
        publishTelemetry(config, outputs, telemetry);
    }
}

bool MqttManager::isConnected() {
    return client_.connected();
}

void MqttManager::flushPendingWork() {
    if (pendingConfigApplied_.active) {
        publishConfigApplied(
            currentConfig_,
            pendingConfigApplied_.requestedVersion,
            pendingConfigApplied_.appliedVersion,
            pendingConfigApplied_.result.c_str(),
            pendingConfigApplied_.message.c_str());
        pendingConfigApplied_ = PendingConfigApplied{};
    }

    if (hasPendingSystemConfig_ && systemConfigHandler_) {
        hasPendingSystemConfig_ = false;
        LOG_DEBUG_MSG("[mqtt] applying queued system_config");
        systemConfigHandler_(pendingSystemConfig_);
    }

    if (hasPendingFermentationConfig_ && fermentationConfigHandler_) {
        hasPendingFermentationConfig_ = false;
        LOG_DEBUG_MSG("[mqtt] applying queued fermentation config");
        fermentationConfigHandler_(pendingFermentationConfig_);
    }

    if (pendingOutputCommand_.active && outputCommandHandler_) {
        const String target = pendingOutputCommand_.target;
        const OutputState state = pendingOutputCommand_.state;
        pendingOutputCommand_ = PendingOutputCommand{};
        LOG_DEBUG(
            "[mqtt] dispatching queued output command target=%s state=%s\r\n",
            target.c_str(),
            outputStateName(state));
        outputCommandHandler_(target, state);
    }

    if (pendingProfileCommand_.active && profileCommandHandler_) {
        const String command = pendingProfileCommand_.command;
        const String stepId = pendingProfileCommand_.stepId;
        pendingProfileCommand_ = PendingProfileCommand{};
        LOG_DEBUG("[mqtt] dispatching queued profile command=%s step_id=%s\r\n", command.c_str(), stepId.c_str());
        profileCommandHandler_(command, stepId);
    }

    if (pendingDiscoveryRequest_ && discoveryRequestHandler_) {
        pendingDiscoveryRequest_ = false;
        LOG_DEBUG_MSG("[mqtt] dispatching queued discovery request");
        discoveryRequestHandler_();
    }

    if (pendingOtaCommand_.active && otaCommandHandler_) {
        const String command = pendingOtaCommand_.command;
        const String channel = pendingOtaCommand_.channel;
        pendingOtaCommand_ = PendingOtaCommand{};
        LOG_DEBUG("[mqtt] dispatching queued ota command=%s channel=%s\r\n", command.c_str(), channel.c_str());
        otaCommandHandler_(command, channel);
    }
}

void MqttManager::setSystemConfigHandler(SystemConfigHandler handler) {
    systemConfigHandler_ = handler;
}

void MqttManager::setFermentationConfigHandler(FermentationConfigHandler handler) {
    fermentationConfigHandler_ = handler;
}

void MqttManager::setOutputCommandHandler(OutputCommandHandler handler) {
    outputCommandHandler_ = handler;
}

void MqttManager::setProfileCommandHandler(ProfileCommandHandler handler) {
    profileCommandHandler_ = handler;
}

void MqttManager::setDiscoveryRequestHandler(DiscoveryRequestHandler handler) {
    discoveryRequestHandler_ = handler;
}

void MqttManager::setAppliedFermentationVersion(uint32_t version) {
    lastAppliedFermentationVersion_ = version;
}

void MqttManager::setOtaCommandHandler(OtaCommandHandler handler) {
    otaCommandHandler_ = handler;
}

bool MqttManager::connectIfNeeded(
    const SystemConfig& config,
    const OutputManager& outputs,
    const LocalUiManager& localUi,
    const TelemetrySnapshot& telemetry) {
    if (client_.connected()) {
        return false;
    }

    const uint32_t now = millis();
    if (now - lastReconnectAttemptMs_ < 5000UL) {
        return false;
    }

    lastReconnectAttemptMs_ = now;

    const String willPayload =
        "{\"device_id\":\"" + config.deviceId + "\",\"status\":\"offline\"}";
    const bool connected = client_.connect(
        config.mqtt.clientId.c_str(),
        config.mqtt.username.c_str(),
        config.mqtt.password.c_str(),
        topicFor(config, "availability").c_str(),
        1,
        true,
        willPayload.c_str());

    if (!connected) {
        LOG_WARN("[mqtt] connect failed rc=%d\r\n", client_.state());
        return false;
    }

    LOG_INFO("[mqtt] connected host=%s port=%u\r\n", config.mqtt.host.c_str(), config.mqtt.port);
    const String systemConfigTopic = topicFor(config, "system_config");
    const String commandTopic = topicFor(config, "command");
    const String configDesiredTopic = topicFor(config, "config/desired");
    client_.subscribe(systemConfigTopic.c_str());
    client_.subscribe(commandTopic.c_str());
    client_.subscribe(configDesiredTopic.c_str());
    LOG_DEBUG("[mqtt] subscribed topic=%s\r\n", systemConfigTopic.c_str());
    LOG_DEBUG("[mqtt] subscribed topic=%s\r\n", commandTopic.c_str());
    LOG_DEBUG("[mqtt] subscribed topic=%s\r\n", configDesiredTopic.c_str());
    publishAvailability(config, "online");
    publishHeartbeat(config, outputs, localUi);
    publishTelemetry(config, outputs, telemetry);
    publishState(config, outputs, localUi, telemetry);
    lastHeartbeatMs_ = millis();
    lastTelemetryMs_ = millis();
    return true;
}

void MqttManager::publishAvailability(const SystemConfig& config, const char* status) {
    const String payload =
        "{\"device_id\":\"" + config.deviceId + "\",\"status\":\"" + status
        + "\",\"fw_version\":\"" + FirmwareVersion::kCurrent + "\"}";
    client_.publish(topicFor(config, "availability").c_str(), payload.c_str(), true);
}

void MqttManager::publishHeartbeat(
    const SystemConfig& config,
    const OutputManager& outputs,
    const LocalUiManager& localUi) {
    char payload[384];
    snprintf(
        payload,
        sizeof(payload),
        "{\"device_id\":\"%s\",\"uptime_s\":%lu,\"wifi_rssi\":%d,"
        "\"heap_free\":%u,\"ui\":\"%s\",\"heating\":\"%s\",\"cooling\":\"%s\"}",
        config.deviceId.c_str(),
        static_cast<unsigned long>(millis() / 1000UL),
        WiFi.RSSI(),
        ESP.getFreeHeap(),
        localUi.isHeadless() ? "headless" : "local",
        outputStateName(outputs.heatingState()),
        outputStateName(outputs.coolingState()));

    client_.publish(topicFor(config, "heartbeat").c_str(), payload, false);
    LOG_DEBUG("[mqtt] heartbeat published topic=%s\r\n", topicFor(config, "heartbeat").c_str());
}

void MqttManager::publishState(
    const SystemConfig& config,
    const OutputManager& outputs,
    const LocalUiManager& localUi,
    const TelemetrySnapshot& telemetry) {
    const String heatingDescription = outputs.describeHeating();
    const String coolingDescription = outputs.describeCooling();

    StaticJsonDocument<1280> doc;
    doc["device_id"] = config.deviceId;
    doc["fw_version"] = FirmwareVersion::kCurrent;
    doc["ui"] = localUi.isHeadless() ? "headless" : "local";
    doc["mode"] = telemetry.mode;
    doc["heating"] = outputStateName(outputs.heatingState());
    doc["cooling"] = outputStateName(outputs.coolingState());
    doc["heating_desc"] = heatingDescription;
    doc["cooling_desc"] = coolingDescription;
    doc["ota_status"] = telemetry.otaStatus;
    doc["ota_channel"] = telemetry.otaChannel;
    doc["ota_available"] = telemetry.otaUpdateAvailable;
    doc["ota_progress_pct"] = telemetry.otaProgressPercent;
    doc["ota_reboot_pending"] = telemetry.otaRebootPending;
    doc["controller_state"] = telemetry.controllerState;
    doc["controller_reason"] = telemetry.controllerReason;
    doc["automatic_control_active"] = telemetry.automaticControlActive;
    doc["active_config_version"] = telemetry.activeConfigVersion;
    doc["secondary_sensor_enabled"] = telemetry.secondarySensorEnabled;
    doc["control_sensor"] = telemetry.controlSensor;
    doc["beer_probe_present"] = telemetry.beerProbePresent;
    doc["beer_probe_valid"] = telemetry.beerProbeValid;
    doc["beer_probe_stale"] = telemetry.beerProbeStale;
    doc["beer_probe_rom"] = telemetry.beerProbeRom;
    doc["chamber_probe_present"] = telemetry.chamberProbePresent;
    doc["chamber_probe_valid"] = telemetry.chamberProbeValid;
    doc["chamber_probe_stale"] = telemetry.chamberProbeStale;
    doc["chamber_probe_rom"] = telemetry.chamberProbeRom;
    if (!telemetry.fault.isEmpty()) {
        doc["fault"] = telemetry.fault;
    } else {
        doc["fault"] = nullptr;
    }
    if (telemetry.hasSetpoint) {
        doc["setpoint_c"] = telemetry.setpointC;
    } else {
        doc["setpoint_c"] = nullptr;
    }
    doc["hysteresis_c"] = telemetry.hysteresisC;
    doc["cooling_delay_s"] = telemetry.coolingDelaySeconds;
    doc["heating_delay_s"] = telemetry.heatingDelaySeconds;
    if (telemetry.hasProfileRuntime) {
        JsonObject runtime = doc.createNestedObject("profile_runtime");
        runtime["active_profile_id"] = telemetry.profileId;
        runtime["active_step_id"] = telemetry.profileStepId;
        runtime["active_step_index"] = telemetry.profileStepIndex;
        runtime["phase"] = telemetry.profilePhase;
        runtime["step_started_at"] = telemetry.stepStartedAtSeconds;
        runtime["step_hold_started_at"] = telemetry.stepHoldStartedAtSeconds;
        runtime["effective_target_c"] = telemetry.effectiveTargetC;
        runtime["waiting_for_manual_release"] = telemetry.waitingForManualRelease;
        runtime["paused"] = telemetry.profilePaused;
    }
    if (!telemetry.otaTargetVersion.isEmpty()) {
        doc["ota_target_version"] = telemetry.otaTargetVersion;
    }
    if (!telemetry.otaMessage.isEmpty()) {
        doc["ota_message"] = telemetry.otaMessage;
    }

    String payload;
    const size_t payloadLength = serializeJson(doc, payload);
    if (payloadLength == 0 || payloadLength >= kMqttBufferSize) {
        LOG_WARN_MSG("[mqtt] state payload too large");
        return;
    }

    client_.publish(topicFor(config, "state").c_str(), payload.c_str(), true);
}

void MqttManager::publishTelemetry(
    const SystemConfig& config,
    const OutputManager& outputs,
    const TelemetrySnapshot& telemetry) {
    StaticJsonDocument<768> doc;
    doc["device_id"] = config.deviceId;
    doc["ts"] = millis() / 1000UL;
    doc["mode"] = telemetry.mode;
    doc["controller_state"] = telemetry.controllerState;
    doc["controller_reason"] = telemetry.controllerReason;
    doc["automatic_control_active"] = telemetry.automaticControlActive;
    doc["active_config_version"] = telemetry.activeConfigVersion;
    doc["secondary_sensor_enabled"] = telemetry.secondarySensorEnabled;
    doc["control_sensor"] = telemetry.controlSensor;
    doc["beer_probe_present"] = telemetry.beerProbePresent;
    doc["beer_probe_valid"] = telemetry.beerProbeValid;
    doc["beer_probe_stale"] = telemetry.beerProbeStale;
    doc["beer_probe_rom"] = telemetry.beerProbeRom;
    doc["chamber_probe_present"] = telemetry.chamberProbePresent;
    doc["chamber_probe_valid"] = telemetry.chamberProbeValid;
    doc["chamber_probe_stale"] = telemetry.chamberProbeStale;
    doc["chamber_probe_rom"] = telemetry.chamberProbeRom;
    if (!telemetry.fault.isEmpty()) {
        doc["fault"] = telemetry.fault;
    } else {
        doc["fault"] = nullptr;
    }
    doc["heating"] = outputs.heatingState() == OutputState::On;
    doc["cooling"] = outputs.coolingState() == OutputState::On;

    if (telemetry.hasPrimaryTemp) {
        doc["temp_primary_c"] = telemetry.primaryTempC;
    } else {
        doc["temp_primary_c"] = nullptr;
    }

    if (telemetry.hasSecondaryTemp) {
        doc["temp_secondary_c"] = telemetry.secondaryTempC;
    } else {
        doc["temp_secondary_c"] = nullptr;
    }

    if (telemetry.hasSetpoint) {
        doc["setpoint_c"] = telemetry.setpointC;
    } else {
        doc["setpoint_c"] = nullptr;
    }

    if (!telemetry.profileId.isEmpty()) {
        doc["profile_id"] = telemetry.profileId;
    }
    if (!telemetry.profileStepId.isEmpty()) {
        doc["profile_step_id"] = telemetry.profileStepId;
    }
    if (telemetry.hasEffectiveTarget) {
        doc["effective_target_c"] = telemetry.effectiveTargetC;
    }

    String payload;
    const size_t payloadLength = serializeJson(doc, payload);
    if (payloadLength == 0 || payloadLength >= kMqttBufferSize) {
        LOG_WARN_MSG("[mqtt] telemetry payload too large");
        return;
    }

    client_.publish(topicFor(config, "telemetry").c_str(), payload.c_str(), false);
}

void MqttManager::publishOutputDiscovery(const SystemConfig& config, const String& devicePayload) {
    if (!client_.connected()) {
        return;
    }

    const String payload = "{\"device_id\":\"" + config.deviceId + "\",\"result\":" + devicePayload + "}";
    client_.publish(topicFor(config, "discovery/output").c_str(), payload.c_str(), false);
}

void MqttManager::publishEvent(
    const SystemConfig& config,
    const char* eventName,
    const char* result,
    const char* message,
    const TelemetrySnapshot& telemetry) {
    if (!client_.connected()) {
        return;
    }

    StaticJsonDocument<384> doc;
    doc["device_id"] = config.deviceId;
    doc["ts"] = millis() / 1000UL;
    doc["event"] = eventName;
    doc["fw_version"] = FirmwareVersion::kCurrent;
    doc["result"] = result;
    doc["ota_status"] = telemetry.otaStatus;
    if (!telemetry.otaTargetVersion.isEmpty()) {
        doc["target_version"] = telemetry.otaTargetVersion;
    }
    if (message != nullptr && message[0] != '\0') {
        doc["message"] = message;
    } else {
        doc["message"] = nullptr;
    }

    char payload[384];
    serializeJson(doc, payload, sizeof(payload));
    client_.publish(topicFor(config, "event").c_str(), payload, false);
}

void MqttManager::publishConfigApplied(
    const SystemConfig& config,
    uint32_t requestedVersion,
    uint32_t appliedVersion,
    const char* result,
    const char* message) {
    StaticJsonDocument<384> doc;
    doc["device_id"] = config.deviceId;
    doc["requested_version"] = requestedVersion;
    doc["applied_version"] = appliedVersion;
    doc["result"] = result;
    if (message != nullptr && message[0] != '\0') {
        doc["message"] = message;
    } else {
        doc["message"] = nullptr;
    }

    char payload[384];
    serializeJson(doc, payload, sizeof(payload));
    client_.publish(topicFor(config, "config/applied").c_str(), payload, false);
    if (strcmp(result, "ok") == 0) {
        lastAppliedFermentationVersion_ = appliedVersion;
    }
}

void MqttManager::handleMessage(
    const SystemConfig& config,
    char* topic,
    uint8_t* payload,
    unsigned int length) {
    String body;
    body.reserve(length);
    for (unsigned int i = 0; i < length; ++i) {
        body += static_cast<char>(payload[i]);
    }

    const String topicName(topic);
    if (topicName.endsWith("/command")) {
        LOG_DEBUG(
            "[mqtt] message topic=%s bytes=%u body=%s\r\n",
            topicName.c_str(),
            length,
            body.c_str());
    } else {
        LOG_DEBUG("[mqtt] message topic=%s bytes=%u\r\n", topicName.c_str(), length);
    }
    if (topicName.endsWith("/system_config")) {
        handleSystemConfig(config, body);
        return;
    }

    if (topicName.endsWith("/config/desired")) {
        handleFermentationConfig(body);
        return;
    }

    if (topicName.endsWith("/command")) {
        handleCommand(body);
    }
}

void MqttManager::handleSystemConfig(const SystemConfig& currentConfig, const String& payload) {
    if (!systemConfigHandler_) {
        return;
    }

    StaticJsonDocument<768> doc;
    const DeserializationError error = deserializeJson(doc, payload);
    if (error) {
        LOG_WARN("[mqtt] invalid system_config json error=%s\r\n", error.c_str());
        return;
    }

    SystemConfig updated = currentConfig;
    if (doc.containsKey("debug_enabled")) {
        updated.debugEnabled = doc["debug_enabled"] | updated.debugEnabled;
    }
    JsonObject heating = doc["heating"];
    if (!heating.isNull()) {
        updated.heatingOutput.driver =
            outputDriverFromString(String(static_cast<const char*>(heating["driver"] | "none")));
        updated.heatingOutput.host = String(static_cast<const char*>(heating["host"] | ""));
        updated.heatingOutput.port = heating["port"] | updated.heatingOutput.port;
        updated.heatingOutput.switchId = heating["switch_id"] | updated.heatingOutput.switchId;
        updated.heatingOutput.alias = String(static_cast<const char*>(heating["alias"] | ""));
    }

    JsonObject cooling = doc["cooling"];
    if (!cooling.isNull()) {
        updated.coolingOutput.driver =
            outputDriverFromString(String(static_cast<const char*>(cooling["driver"] | "none")));
        updated.coolingOutput.host = String(static_cast<const char*>(cooling["host"] | ""));
        updated.coolingOutput.port = cooling["port"] | updated.coolingOutput.port;
        updated.coolingOutput.switchId = cooling["switch_id"] | updated.coolingOutput.switchId;
        updated.coolingOutput.alias = String(static_cast<const char*>(cooling["alias"] | ""));
    }

    if (doc.containsKey("heartbeat_interval_s")) {
        updated.heartbeat.intervalSeconds = doc["heartbeat_interval_s"] | updated.heartbeat.intervalSeconds;
    }

    JsonObject ota = doc["ota"];
    if (!ota.isNull()) {
        updated.ota.enabled = ota["enabled"] | updated.ota.enabled;
        updated.ota.channel =
            String(static_cast<const char*>(ota["channel"] | updated.ota.channel.c_str()));
        updated.ota.checkStrategy = String(
            static_cast<const char*>(ota["check_strategy"] | updated.ota.checkStrategy.c_str()));
        updated.ota.checkIntervalSeconds =
            ota["check_interval_s"] | updated.ota.checkIntervalSeconds;
        updated.ota.manifestUrl = String(
            static_cast<const char*>(ota["manifest_url"] | updated.ota.manifestUrl.c_str()));
        updated.ota.caCertFingerprint = String(
            static_cast<const char*>(ota["ca_cert_fingerprint"] | updated.ota.caCertFingerprint.c_str()));
        updated.ota.allowHttp = ota["allow_http"] | updated.ota.allowHttp;
    }

    currentConfig_ = updated;
    pendingSystemConfig_ = updated;
    hasPendingSystemConfig_ = true;
    LOG_DEBUG_MSG("[mqtt] queued system_config update");
}

void MqttManager::handleFermentationConfig(const String& payload) {
    if (!fermentationConfigHandler_) {
        return;
    }

    LOG_DEBUG("[mqtt] fermentation config received bytes=%u\r\n", payload.length());

    DynamicJsonDocument doc(4096);
    const DeserializationError error = deserializeJson(doc, payload);
    if (error) {
        LOG_WARN("[mqtt] invalid fermentation config json error=%s\r\n", error.c_str());
        return;
    }

    const uint32_t schemaVersion = doc["schema_version"] | 0;
    const uint32_t version = doc["version"] | 0;
    const String deviceId = String(static_cast<const char*>(doc["device_id"] | ""));
    const String mode = String(static_cast<const char*>(doc["mode"] | ""));
    JsonObject thermostat = doc["thermostat"];
    JsonObject sensors = doc["sensors"];

    JsonObject profile = doc["profile"];
    if (schemaVersion != 2 || version == 0 || deviceId.isEmpty() || thermostat.isNull() || sensors.isNull()) {
        LOG_WARN_MSG("[mqtt] fermentation config missing required fields");
        pendingConfigApplied_.active = true;
        pendingConfigApplied_.requestedVersion = version;
        pendingConfigApplied_.appliedVersion = lastAppliedFermentationVersion_;
        pendingConfigApplied_.result = "error";
        pendingConfigApplied_.message = "missing required fields";
        return;
    }

    const float setpointC = thermostat["setpoint_c"] | NAN;
    const float hysteresisC = thermostat["hysteresis_c"] | NAN;
    const uint16_t coolingDelaySeconds = thermostat["cooling_delay_s"] | 0;
    const uint16_t heatingDelaySeconds = thermostat["heating_delay_s"] | 0;
    const float primaryOffsetC = sensors["primary_offset_c"] | NAN;
    const bool secondaryEnabled = sensors["secondary_enabled"] | false;
    const float secondaryOffsetC = sensors["secondary_offset_c"] | 0.0f;
    const float secondaryLimitHysteresisC = sensors["secondary_limit_hysteresis_c"] | NAN;
    const String controlSensor = String(static_cast<const char*>(sensors["control_sensor"] | "primary"));

    if (
        isnan(setpointC) || setpointC < -20.0f || setpointC > 50.0f || isnan(hysteresisC)
        || hysteresisC < 0.1f || hysteresisC > 5.0f || coolingDelaySeconds > 3600
        || heatingDelaySeconds > 3600 || isnan(primaryOffsetC) || primaryOffsetC < -5.0f
        || primaryOffsetC > 5.0f || (secondaryEnabled && (isnan(secondaryLimitHysteresisC)
        || secondaryLimitHysteresisC < 0.1f || secondaryLimitHysteresisC > 25.0f))
        || (controlSensor != "primary" && controlSensor != "secondary")
        || (controlSensor == "secondary" && !secondaryEnabled)
        || (mode != "thermostat" && mode != "profile")) {
        LOG_WARN_MSG("[mqtt] fermentation config validation failed");
        pendingConfigApplied_.active = true;
        pendingConfigApplied_.requestedVersion = version;
        pendingConfigApplied_.appliedVersion = lastAppliedFermentationVersion_;
        pendingConfigApplied_.result = "error";
        pendingConfigApplied_.message = "thermostat or sensor validation failed";
        return;
    }

    FermentationConfig updated;
    updated.schemaVersion = schemaVersion;
    updated.version = version;
    updated.deviceId = deviceId;
    updated.name = String(static_cast<const char*>(doc["name"] | "Default fermentation"));
    updated.mode = mode;
    updated.thermostat.setpointC = setpointC;
    updated.thermostat.hysteresisC = hysteresisC;
    updated.thermostat.coolingDelaySeconds = coolingDelaySeconds;
    updated.thermostat.heatingDelaySeconds = heatingDelaySeconds;
    updated.sensors.primaryOffsetC = primaryOffsetC;
    updated.sensors.secondaryEnabled = secondaryEnabled;
    updated.sensors.secondaryOffsetC = secondaryOffsetC;
    updated.sensors.secondaryLimitHysteresisC =
        secondaryEnabled ? secondaryLimitHysteresisC : 1.5f;
    updated.sensors.controlSensor = controlSensor;

    if (mode == "profile") {
        if (profile.isNull()) {
            pendingConfigApplied_.active = true;
            pendingConfigApplied_.requestedVersion = version;
            pendingConfigApplied_.appliedVersion = lastAppliedFermentationVersion_;
            pendingConfigApplied_.result = "error";
            pendingConfigApplied_.message = "profile payload required for profile mode";
            return;
        }

        updated.profile.id = String(static_cast<const char*>(profile["id"] | ""));
        JsonArray steps = profile["steps"].as<JsonArray>();
        if (updated.profile.id.isEmpty() || steps.isNull() || steps.size() == 0 || steps.size() > kMaxProfileSteps) {
            pendingConfigApplied_.active = true;
            pendingConfigApplied_.requestedVersion = version;
            pendingConfigApplied_.appliedVersion = lastAppliedFermentationVersion_;
            pendingConfigApplied_.result = "error";
            pendingConfigApplied_.message = "profile must include 1-10 steps";
            return;
        }

        updated.profile.stepCount = 0;
        for (JsonObject step : steps) {
            const String stepId = String(static_cast<const char*>(step["id"] | ""));
            const String stepLabel = String(static_cast<const char*>(step["label"] | ""));
            const float targetC = step["target_c"] | NAN;
            const uint32_t holdDurationSeconds = step["hold_duration_s"] | UINT32_MAX;
            const uint32_t rampDurationSeconds = step["ramp_duration_s"] | 0;
            const String advancePolicy = String(static_cast<const char*>(step["advance_policy"] | ""));

            if (
                stepId.isEmpty() || isnan(targetC) || targetC < -20.0f || targetC > 50.0f
                || holdDurationSeconds > 3596400UL || rampDurationSeconds > 3596400UL
                || (advancePolicy != "auto" && advancePolicy != "manual_release")) {
                pendingConfigApplied_.active = true;
                pendingConfigApplied_.requestedVersion = version;
                pendingConfigApplied_.appliedVersion = lastAppliedFermentationVersion_;
                pendingConfigApplied_.result = "error";
                pendingConfigApplied_.message = "profile step validation failed";
                return;
            }

            ProfileStepConfig& entry = updated.profile.steps[updated.profile.stepCount++];
            entry.id = stepId;
            entry.label = stepLabel;
            entry.targetC = targetC;
            entry.holdDurationSeconds = holdDurationSeconds;
            entry.rampDurationSeconds = rampDurationSeconds;
            entry.advancePolicy = advancePolicy;
        }
    }

    LOG_DEBUG(
        "[mqtt] fermentation config validated version=%lu mode=%s\r\n",
        static_cast<unsigned long>(version),
        updated.mode.c_str());
    pendingFermentationConfig_ = updated;
    hasPendingFermentationConfig_ = true;
}

void MqttManager::handleCommand(const String& payload) {
    LOG_DEBUG("[mqtt] command payload=%s\r\n", payload.c_str());

    StaticJsonDocument<384> doc;
    const DeserializationError error = deserializeJson(doc, payload);
    if (error) {
        LOG_WARN("[mqtt] invalid command json error=%s\r\n", error.c_str());
        return;
    }

    const String command = String(static_cast<const char*>(doc["command"] | ""));
    LOG_DEBUG("[mqtt] command parsed command=%s\r\n", command.c_str());
    if (command == "discover_kasa" || command == "discover_outputs") {
        LOG_DEBUG(
            "[mqtt] discovery command received command=%s handler=%s\r\n",
            command.c_str(),
            discoveryRequestHandler_ ? "set" : "missing");
        if (discoveryRequestHandler_) {
            pendingDiscoveryRequest_ = true;
            LOG_DEBUG_MSG("[mqtt] queued discovery request");
        } else {
            LOG_WARN_MSG("[mqtt] discovery command ignored because handler is missing");
        }
        return;
    }

    if (command == "set_output" && outputCommandHandler_) {
        const String target = String(static_cast<const char*>(doc["target"] | ""));
        const String state = String(static_cast<const char*>(doc["state"] | ""));
        LOG_DEBUG("[mqtt] output command target=%s state=%s\r\n", target.c_str(), state.c_str());
        pendingOutputCommand_.active = true;
        pendingOutputCommand_.target = target;
        pendingOutputCommand_.state = outputStateFromString(state);
        LOG_DEBUG_MSG("[mqtt] queued output command");
        return;
    }

    if (profileCommandHandler_ && (
        command == "profile_pause" || command == "profile_resume" || command == "profile_release_hold"
        || command == "profile_jump_to_step" || command == "profile_stop")) {
        String stepId;
        JsonObject args = doc["args"];
        if (!args.isNull()) {
            stepId = String(static_cast<const char*>(args["step_id"] | ""));
        }
        LOG_DEBUG("[mqtt] profile command=%s step_id=%s\r\n", command.c_str(), stepId.c_str());
        pendingProfileCommand_.active = true;
        pendingProfileCommand_.command = command;
        pendingProfileCommand_.stepId = stepId;
        LOG_DEBUG_MSG("[mqtt] queued profile command");
        return;
    }

    if ((command == "check_update" || command == "start_update") && otaCommandHandler_) {
        JsonObject args = doc["args"];
        const String channel = args.isNull()
            ? String(static_cast<const char*>(doc["channel"] | ""))
            : String(static_cast<const char*>(args["channel"] | ""));
        LOG_DEBUG("[mqtt] ota command=%s channel=%s\r\n", command.c_str(), channel.c_str());
        pendingOtaCommand_.active = true;
        pendingOtaCommand_.command = command;
        pendingOtaCommand_.channel = channel;
        LOG_DEBUG_MSG("[mqtt] queued ota command");
        return;
    }

    LOG_WARN("[mqtt] unhandled command=%s\r\n", command.c_str());
}

String MqttManager::topicFor(const SystemConfig& config, const char* suffix) const {
    return config.mqtt.topicPrefix + "/" + config.deviceId + "/" + suffix;
}
