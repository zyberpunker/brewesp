#include "App.h"

#include <WiFi.h>

namespace {
const uint32_t kHeartbeatLogIntervalMs = 5000;
constexpr float kDefaultSetpointC = 20.0f;
}

void App::begin() {
    config_ = buildDefaultConfig();
    configStore_.load(config_);
    fermentationConfig_ = buildDefaultFermentationConfig(config_.deviceId);
    configStore_.loadFermentationConfig(fermentationConfig_);
    initializeProfileRuntime();
    mqtt_.setAppliedFermentationVersion(fermentationConfig_.version);

    Serial.println();
    Serial.println("brewesp firmware booting");
    Serial.printf("device_id=%s\r\n", config_.deviceId.c_str());

    mqtt_.setSystemConfigHandler([this](const SystemConfig& updatedConfig) { handleSystemConfig(updatedConfig); });
    mqtt_.setFermentationConfigHandler(
        [this](const FermentationConfig& updatedConfig) { handleFermentationConfig(updatedConfig); });
    mqtt_.setOutputCommandHandler(
        [this](const String& target, OutputState state) { handleOutputCommand(target, state); });
    mqtt_.setProfileCommandHandler(
        [this](const String& command, const String& stepId) { handleProfileCommand(command, stepId); });
    mqtt_.setDiscoveryRequestHandler([this]() { runKasaDiscovery(); });

    localUi_.begin(config_);
    outputs_.begin(config_);
    sensors_.begin(config_);
    controller_.reset();

    if (config_.wifi.ssid.isEmpty()) {
        startProvisioningMode("missing Wi-Fi config");
        return;
    }

    beginNormalMode();
}

void App::update() {
    if (provisioningMode_) {
        provisioning_.update();
        if (provisioning_.restartRequested()) {
            Serial.println("[prov] restarting after configuration save");
            delay(1000);
            ESP.restart();
        }
        return;
    }

    ensureWifiConnected();
    localUi_.update();
    outputs_.update();
    sensors_.update(config_);
    if (controller_.update(fermentationConfig_, buildControllerInputs(), outputs_)) {
        mqtt_.publishState(config_, outputs_, localUi_, buildTelemetrySnapshot());
    }
    mqtt_.update(config_, outputs_, localUi_, buildTelemetrySnapshot());

    const uint32_t now = millis();
    if (now - lastHeartbeatLogMs_ < kHeartbeatLogIntervalMs) {
        return;
    }

    lastHeartbeatLogMs_ = now;

    Serial.printf(
        "heartbeat ui=%s wifi=%s mqtt=%s heating=%s cooling=%s\r\n",
        localUi_.isHeadless() ? "headless" : "local",
        WiFi.status() == WL_CONNECTED ? "connected" : "disconnected",
        mqtt_.isConnected() ? "connected" : "disconnected",
        outputs_.describeHeating().c_str(),
        outputs_.describeCooling().c_str());
}

void App::beginNormalMode() {
    provisioningMode_ = false;
    WiFi.mode(WIFI_STA);
    ensureWifiConnected();
    mqtt_.begin(config_);
}

void App::handleSystemConfig(const SystemConfig& updatedConfig) {
    config_.sensors = updatedConfig.sensors;
    config_.heatingOutput = updatedConfig.heatingOutput;
    config_.coolingOutput = updatedConfig.coolingOutput;
    config_.heartbeat = updatedConfig.heartbeat;
    configStore_.save(config_);
    sensors_.begin(config_);
    outputs_.applyConfig(config_);
    mqtt_.publishState(config_, outputs_, localUi_, buildTelemetrySnapshot());
    Serial.println("[app] applied system_config from MQTT");
}

void App::handleFermentationConfig(const FermentationConfig& updatedConfig) {
    fermentationConfigRollback_ = fermentationConfig_;
    profileRuntimeRollback_ = profileRuntime_;

    if (updatedConfig.deviceId != config_.deviceId) {
        mqtt_.publishConfigApplied(
            config_,
            updatedConfig.version,
            fermentationConfig_.version,
            "error",
            "device_id mismatch");
        return;
    }

    fermentationConfig_ = updatedConfig;
    initializeProfileRuntime();
    if (!configStore_.saveFermentationConfig(fermentationConfig_)) {
        fermentationConfig_ = fermentationConfigRollback_;
        profileRuntime_ = profileRuntimeRollback_;
        mqtt_.publishConfigApplied(
            config_,
            updatedConfig.version,
            fermentationConfigRollback_.version,
            "error",
            "failed to persist fermentation config");
        return;
    }

    controller_.reset();
    mqtt_.publishState(config_, outputs_, localUi_, buildTelemetrySnapshot());
    mqtt_.publishConfigApplied(
        config_,
        fermentationConfig_.version,
        fermentationConfig_.version,
        "ok",
        nullptr);
    Serial.printf(
        "[app] applied fermentation config version=%lu mode=%s setpoint=%.2f hysteresis=%.2f\r\n",
        static_cast<unsigned long>(fermentationConfig_.version),
        fermentationConfig_.mode.c_str(),
        fermentationConfig_.thermostat.setpointC,
        fermentationConfig_.thermostat.hysteresisC);
}

void App::handleOutputCommand(const String& target, OutputState state) {
    if (state == OutputState::Unknown) {
        return;
    }

    Serial.printf(
        "[app] output command target=%s state=%s\r\n",
        target.c_str(),
        state == OutputState::On ? "on" : "off");

    bool changed = false;
    if (target == "heating") {
        changed = outputs_.setHeating(state);
    } else if (target == "cooling") {
        changed = outputs_.setCooling(state);
    } else if (target == "all") {
        changed = outputs_.setHeating(state) || outputs_.setCooling(state);
    }

    outputs_.refreshStates();
    Serial.printf(
        "[app] output command result changed=%s heating=%s cooling=%s\r\n",
        changed ? "true" : "false",
        outputs_.describeHeating().c_str(),
        outputs_.describeCooling().c_str());
    if (changed) {
        mqtt_.publishState(config_, outputs_, localUi_, buildTelemetrySnapshot());
    }
}

void App::handleProfileCommand(const String& command, const String& stepId) {
    if (fermentationConfig_.mode != "profile" || fermentationConfig_.profile.stepCount == 0) {
        return;
    }

    fermentationConfigRollback_ = fermentationConfig_;
    profileRuntimeRollback_ = profileRuntime_;
    bool changed = false;

    if (command == "profile_pause") {
        profileRuntime_.paused = true;
        profileRuntime_.phase = "paused";
        changed = true;
    } else if (command == "profile_resume") {
        profileRuntime_.paused = false;
        if (profileRuntime_.waitingForManualRelease) {
            profileRuntime_.phase = "waiting_manual_release";
        } else {
            profileRuntime_.phase = "holding";
        }
        changed = true;
    } else if (command == "profile_release_hold") {
        if (profileRuntime_.waitingForManualRelease) {
            const int nextStep = profileRuntime_.activeStepIndex + 1;
            if (nextStep >= fermentationConfig_.profile.stepCount) {
                profileRuntime_.waitingForManualRelease = false;
                profileRuntime_.phase = "completed";
                changed = true;
            } else {
                changed = activateProfileStep(static_cast<uint8_t>(nextStep), true);
            }
        }
    } else if (command == "profile_jump_to_step") {
        for (uint8_t i = 0; i < fermentationConfig_.profile.stepCount; ++i) {
            if (fermentationConfig_.profile.steps[i].id == stepId) {
                changed = activateProfileStep(i, true);
                break;
            }
        }
    } else if (command == "profile_stop") {
        fermentationConfig_.mode = "thermostat";
        resetProfileRuntime();
        changed = true;
    }

    if (!changed) {
        return;
    }

    if (!configStore_.saveFermentationConfig(fermentationConfig_)) {
        fermentationConfig_ = fermentationConfigRollback_;
        profileRuntime_ = profileRuntimeRollback_;
        return;
    }

    mqtt_.publishState(config_, outputs_, localUi_, buildTelemetrySnapshot());
}

MqttManager::TelemetrySnapshot App::buildTelemetrySnapshot() const {
    MqttManager::TelemetrySnapshot telemetry;
    const ControllerEngine::Status& controllerStatus = controller_.status();
    const SensorManager::Reading& beerProbe = sensors_.beerProbe();
    const SensorManager::Reading& chamberProbe = sensors_.chamberProbe();
    telemetry.hasSetpoint = true;
    telemetry.setpointC = fermentationConfig_.thermostat.setpointC;
    telemetry.hysteresisC = fermentationConfig_.thermostat.hysteresisC;
    telemetry.coolingDelaySeconds = fermentationConfig_.thermostat.coolingDelaySeconds;
    telemetry.heatingDelaySeconds = fermentationConfig_.thermostat.heatingDelaySeconds;
    telemetry.mode = fermentationConfig_.mode;
    telemetry.activeConfigVersion = fermentationConfig_.version;
    telemetry.controllerState = ControllerEngine::stateName(controllerStatus.state);
    telemetry.controllerReason = controllerStatus.reason;
    telemetry.automaticControlActive = controllerStatus.automaticControlActive;
    telemetry.secondarySensorEnabled = fermentationConfig_.sensors.secondaryEnabled;
    telemetry.controlSensor = fermentationConfig_.sensors.controlSensor;
    if (beerProbe.valid) {
        telemetry.hasPrimaryTemp = true;
        telemetry.primaryTempC = beerProbe.tempC + fermentationConfig_.sensors.primaryOffsetC;
    }
    telemetry.beerProbePresent = beerProbe.present;
    telemetry.beerProbeValid = beerProbe.valid;
    if (beerProbe.present) {
        telemetry.beerProbeRom = sensors_.beerProbeRom();
    }
    if (chamberProbe.valid) {
        telemetry.hasSecondaryTemp = true;
        telemetry.secondaryTempC = chamberProbe.tempC + fermentationConfig_.sensors.secondaryOffsetC;
    }
    telemetry.chamberProbePresent = chamberProbe.present;
    telemetry.chamberProbeValid = chamberProbe.valid;
    if (chamberProbe.present) {
        telemetry.chamberProbeRom = sensors_.chamberProbeRom();
    }
    if (profileRuntime_.active) {
        telemetry.hasProfileRuntime = true;
        telemetry.profileId = profileRuntime_.activeProfileId;
        telemetry.profileStepId = profileRuntime_.activeStepId;
        telemetry.profileStepIndex = profileRuntime_.activeStepIndex;
        telemetry.profilePhase = profileRuntime_.phase;
        telemetry.profilePaused = profileRuntime_.paused;
        telemetry.waitingForManualRelease = profileRuntime_.waitingForManualRelease;
        telemetry.hasEffectiveTarget = true;
        telemetry.effectiveTargetC = profileRuntime_.effectiveTargetC;
        telemetry.stepStartedAtSeconds = profileRuntime_.stepStartedMs / 1000UL;
        telemetry.stepHoldStartedAtSeconds = profileRuntime_.stepHoldStartedMs / 1000UL;
    }
    return telemetry;
}

ControllerEngine::Inputs App::buildControllerInputs() const {
    ControllerEngine::Inputs inputs;
    const SensorManager::Reading& beerProbe = sensors_.beerProbe();
    const SensorManager::Reading& chamberProbe = sensors_.chamberProbe();
    inputs.nowMs = millis();
    const bool useSecondaryAsControl =
        fermentationConfig_.sensors.secondaryEnabled && fermentationConfig_.sensors.controlSensor == "secondary";

    if (useSecondaryAsControl) {
        inputs.hasPrimaryTemp = chamberProbe.valid;
        inputs.primaryTempC = chamberProbe.tempC + fermentationConfig_.sensors.secondaryOffsetC;
        inputs.hasSecondaryTemp = beerProbe.valid;
        inputs.secondaryTempC = beerProbe.tempC + fermentationConfig_.sensors.primaryOffsetC;
    } else {
        inputs.hasPrimaryTemp = beerProbe.valid;
        inputs.primaryTempC = beerProbe.tempC + fermentationConfig_.sensors.primaryOffsetC;
        inputs.hasSecondaryTemp = chamberProbe.valid;
        inputs.secondaryTempC = chamberProbe.tempC + fermentationConfig_.sensors.secondaryOffsetC;
    }
    return inputs;
}

FermentationConfig App::buildDefaultFermentationConfig(const String& deviceId) const {
    FermentationConfig config;
    config.schemaVersion = 2;
    config.deviceId = deviceId;
    config.name = "Default fermentation";
    config.mode = "thermostat";
    config.thermostat.setpointC = kDefaultSetpointC;
    config.thermostat.hysteresisC = 0.3f;
    config.thermostat.coolingDelaySeconds = 300;
    config.thermostat.heatingDelaySeconds = 120;
    config.sensors.primaryOffsetC = 0.0f;
    config.sensors.secondaryEnabled = false;
    config.sensors.secondaryOffsetC = 0.0f;
    config.sensors.secondaryLimitHysteresisC = 1.5f;
    config.sensors.controlSensor = "primary";
    return config;
}

void App::resetProfileRuntime() {
    profileRuntime_ = ProfileRuntimeState{};
}

void App::initializeProfileRuntime() {
    resetProfileRuntime();

    if (fermentationConfig_.mode != "profile" || fermentationConfig_.profile.stepCount == 0) {
        return;
    }

    activateProfileStep(0, true);
}

bool App::activateProfileStep(uint8_t stepIndex, bool treatAsFreshStep) {
    if (stepIndex >= fermentationConfig_.profile.stepCount) {
        return false;
    }

    const ProfileStepConfig& step = fermentationConfig_.profile.steps[stepIndex];
    profileRuntime_.active = true;
    profileRuntime_.activeProfileId = fermentationConfig_.profile.id;
    profileRuntime_.activeStepId = step.id;
    profileRuntime_.activeStepIndex = stepIndex;
    profileRuntime_.paused = false;
    profileRuntime_.waitingForManualRelease = step.advancePolicy == "manual_release";
    profileRuntime_.phase = profileRuntime_.waitingForManualRelease ? "waiting_manual_release" : "holding";
    profileRuntime_.effectiveTargetC = step.targetC;
    if (treatAsFreshStep) {
        profileRuntime_.stepStartedMs = millis();
        profileRuntime_.stepHoldStartedMs = millis();
    }

    fermentationConfig_.mode = "profile";
    fermentationConfig_.thermostat.setpointC = step.targetC;
    return true;
}

void App::runKasaDiscovery() {
    Serial.println("[app] running Kasa discovery");
    const bool found = kasaDiscovery_.discover(
        [this](const String& payload) { mqtt_.publishKasaDiscovery(config_, payload); });
    if (!found) {
        Serial.println("[app] Kasa discovery returned no devices");
    }
}

void App::startProvisioningMode(const char* reason) {
    provisioningMode_ = true;
    Serial.printf("[prov] starting provisioning mode reason=%s\r\n", reason);
    provisioning_.begin(
        config_,
        [this](const SystemConfig& updatedConfig) {
            config_ = updatedConfig;
            return configStore_.save(config_);
        });
}

void App::ensureWifiConnected() {
    if (WiFi.status() == WL_CONNECTED) {
        return;
    }

    if (wifiConnectStartedMs_ == 0) {
        wifiConnectStartedMs_ = millis();
        Serial.printf("[wifi] connecting ssid=%s\r\n", config_.wifi.ssid.c_str());
        WiFi.begin(config_.wifi.ssid.c_str(), config_.wifi.password.c_str());
        return;
    }

    if (millis() - wifiConnectStartedMs_
        < config_.wifi.recoveryAp.startAfterWifiFailureSeconds * 1000UL) {
        return;
    }

    if (config_.wifi.recoveryAp.enabled) {
        startProvisioningMode("Wi-Fi connect timeout");
    }
}

SystemConfig App::buildDefaultConfig() const {
    SystemConfig config;
    config.deviceId = "brewesp-dev";
    config.wifi.recoveryAp.password = "brewesp123";
    config.wifi.recoveryAp.ip = "192.168.4.1";
    config.wifi.recoveryAp.startAfterWifiFailureSeconds = 180;

    config.mqtt.port = 1883;
    config.mqtt.clientId = config.deviceId;
    config.mqtt.topicPrefix = "brewesp";
    config.heartbeat.intervalSeconds = 60;
    config.sensors.oneWireGpio = 32;
    config.sensors.pollIntervalSeconds = 5;

    config.localUi.enabled = false;
    config.localUi.headlessAllowed = true;
    config.display.enabled = false;
    config.buttons.enabled = false;

    config.heatingOutput.driver = OutputDriverType::KasaLocal;
    config.heatingOutput.host = "";
    config.heatingOutput.port = 9999;
    config.heatingOutput.alias = "heating-plug";
    config.heatingOutput.pollIntervalSeconds = 30;

    config.coolingOutput.driver = OutputDriverType::KasaLocal;
    config.coolingOutput.host = "";
    config.coolingOutput.port = 9999;
    config.coolingOutput.alias = "cooling-plug";
    config.coolingOutput.pollIntervalSeconds = 30;

    return config;
}
