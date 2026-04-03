#include "App.h"

#include <WiFi.h>

#include "support/Logger.h"

namespace {
const uint32_t kHeartbeatLogIntervalMs = 5000;
constexpr float kDefaultSetpointC = 20.0f;
constexpr uint32_t kProfileRuntimePersistIntervalMs = 300000UL;
constexpr uint32_t kSensorStaleAfterMs = 30000UL;
constexpr uint32_t kManualOutputOverrideDurationMs = 120000UL;
constexpr uint32_t kControlLoopIntervalMs = 1000UL;

bool isSensorStale(const SensorManager::Reading& reading, uint32_t nowMs) {
    return reading.updatedAtMs != 0 && static_cast<uint32_t>(nowMs - reading.updatedAtMs) > kSensorStaleAfterMs;
}

String sensorFaultReason(const char* sensorName, const SensorManager::Reading& reading, bool stale) {
    if (stale) {
        return String(sensorName) + " sensor stale";
    }
    if (!reading.present) {
        return String(sensorName) + " sensor missing";
    }
    return String(sensorName) + " sensor invalid";
}
}

void App::begin() {
    config_ = buildDefaultConfig();
    configStore_.load(config_);
    fermentationConfig_ = buildDefaultFermentationConfig(config_.deviceId);
    configStore_.loadFermentationConfig(fermentationConfig_);
    initializeProfileRuntime();
    persistProfileRuntime(millis(), true);
    mqtt_.setAppliedFermentationVersion(fermentationConfig_.version);
    Logger::setDebugEnabled(config_.debugEnabled);

    Serial.println();
    Serial.println("brewesp firmware booting");
    Serial.printf("device_id=%s\r\n", config_.deviceId.c_str());
    LOG_INFO("[log] debug=%s\r\n", config_.debugEnabled ? "enabled" : "disabled");

    mqtt_.setSystemConfigHandler([this](const SystemConfig& updatedConfig) { handleSystemConfig(updatedConfig); });
    mqtt_.setFermentationConfigHandler(
        [this](const FermentationConfig& updatedConfig) { handleFermentationConfig(updatedConfig); });
    mqtt_.setOutputCommandHandler(
        [this](const String& target, OutputState state) { handleOutputCommand(target, state); });
    mqtt_.setProfileCommandHandler(
        [this](const String& command, const String& stepId) { handleProfileCommand(command, stepId); });
    mqtt_.setDiscoveryRequestHandler([this]() {
        LOG_DEBUG_MSG("[app] discovery request handler invoked");
        runOutputDiscovery();
    });
    mqtt_.setOtaCommandHandler(
        [this](const String& command, const String& channel) { handleOtaCommand(command, channel); });

    localUi_.begin(config_);
    outputs_.begin(config_);
    sensors_.begin(config_);
    controller_.reset();
    ota_.begin(config_);

    if (config_.wifi.ssid.isEmpty()) {
        startProvisioningMode("missing Wi-Fi config");
    } else {
        beginNormalMode();
    }
}

void App::update() {
    if (provisioningMode_) {
        provisioning_.update();
        if (provisioning_.restartRequested()) {
            Serial.println("[prov] restarting after configuration save");
            delay(1000);
            ESP.restart();
        }
    }

    ensureWifiConnected();
    const uint32_t nowMs = millis();
    mqtt_.update(config_, outputs_, localUi_, buildTelemetrySnapshot());
    processPendingOtaCommand();
    updateControlLoop(nowMs);
    if (ota_.shouldRunScheduledCheck(config_, nowMs)) {
        OtaManager::CheckResult check = ota_.checkForUpdate(config_);
        if (mqtt_.isConnected()) {
            mqtt_.publishState(config_, outputs_, localUi_, buildTelemetrySnapshot());
            mqtt_.publishEvent(
                config_,
                "ota_check_completed",
                check.success ? (check.updateAvailable ? "update_available" : "no_update") : "error",
                check.message.c_str(),
                buildTelemetrySnapshot());
        }
    }
    if (otaRestartAtMs_ != 0 && nowMs >= otaRestartAtMs_) {
        LOG_INFO_MSG("[ota] rebooting into staged firmware");
        delay(250);
        otaRestartAtMs_ = 0;
        otaShutdownPending_ = false;
        ESP.restart();
    }

    if (nowMs - lastHeartbeatLogMs_ < kHeartbeatLogIntervalMs) {
        return;
    }

    lastHeartbeatLogMs_ = nowMs;

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

void App::updateControlLoop(uint32_t nowMs) {
    localUi_.update();
    outputs_.update();
    sensors_.update(config_);

    if (lastControlLoopMs_ != 0 && (nowMs - lastControlLoopMs_) < kControlLoopIntervalMs) {
        return;
    }
    lastControlLoopMs_ = nowMs;

    const ControllerEngine::Inputs controllerInputs = buildControllerInputs();

    if (manualOutputOverrideActive_) {
        if (isOtaLockoutActive()) {
            const bool changed = outputs_.setHeating(OutputState::Off) || outputs_.setCooling(OutputState::Off);
            outputs_.refreshStates();
            clearManualOutputOverride("ota lockout");
            if (changed) {
                mqtt_.publishState(config_, outputs_, localUi_, buildTelemetrySnapshot());
            }
            return;
        }

        if (!controllerInputs.hasPrimaryTemp) {
            const bool changed = outputs_.setHeating(OutputState::Off) || outputs_.setCooling(OutputState::Off);
            outputs_.refreshStates();
            clearManualOutputOverride(
                controllerInputs.faultReason.isEmpty() ? "sensor fault" : controllerInputs.faultReason.c_str());
            if (changed) {
                mqtt_.publishState(config_, outputs_, localUi_, buildTelemetrySnapshot());
            }
            return;
        }

        if (
            manualOutputOverrideUntilMs_ != 0
            && static_cast<int32_t>(nowMs - manualOutputOverrideUntilMs_) >= 0) {
            clearManualOutputOverride("timeout");
        } else {
            outputs_.refreshStates();
            return;
        }
    }

    const bool profileCanAdvance = !isOtaLockoutActive() && controllerInputs.hasPrimaryTemp;
    const bool profileChanged = updateProfileRuntime(nowMs, profileCanAdvance);
    if (isOtaLockoutActive()) {
        outputs_.setHeating(OutputState::Off);
        outputs_.setCooling(OutputState::Off);
        outputs_.refreshStates();
    } else if (profileChanged || controller_.update(fermentationConfig_, controllerInputs, outputs_)) {
        mqtt_.publishState(config_, outputs_, localUi_, buildTelemetrySnapshot());
    }
}

void App::handleSystemConfig(const SystemConfig& updatedConfig) {
    if (manualOutputOverrideActive_) {
        clearManualOutputOverride("system_config update");
    }
    config_.debugEnabled = updatedConfig.debugEnabled;
    Logger::setDebugEnabled(config_.debugEnabled);
    config_.sensors = updatedConfig.sensors;
    config_.heatingOutput = updatedConfig.heatingOutput;
    config_.coolingOutput = updatedConfig.coolingOutput;
    config_.heartbeat = updatedConfig.heartbeat;
    config_.ota = updatedConfig.ota;
    configStore_.save(config_);
    sensors_.begin(config_);
    outputs_.applyConfig(config_);
    ota_.begin(config_);
    mqtt_.publishState(config_, outputs_, localUi_, buildTelemetrySnapshot());
    LOG_INFO_MSG("[app] applied system_config from MQTT");
}

void App::handleFermentationConfig(const FermentationConfig& updatedConfig) {
    if (manualOutputOverrideActive_) {
        clearManualOutputOverride("fermentation config update");
    }
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

    if (!configStore_.saveFermentationConfig(updatedConfig)) {
        mqtt_.publishConfigApplied(
            config_,
            updatedConfig.version,
            fermentationConfigRollback_.version,
            "error",
            "failed to persist fermentation config");
        return;
    }

    fermentationConfig_ = updatedConfig;
    initializeProfileRuntime();
    controller_.reset();
    if (fermentationConfig_.mode == "profile") {
        updateProfileRuntime(millis(), true);
        persistProfileRuntime(millis(), true);
    } else {
        clearPersistedProfileRuntime();
    }
    mqtt_.publishState(config_, outputs_, localUi_, buildTelemetrySnapshot());
    mqtt_.publishConfigApplied(
        config_,
        fermentationConfig_.version,
        fermentationConfig_.version,
        "ok",
        nullptr);
    LOG_INFO(
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

    const ControllerEngine::Inputs currentInputs = buildControllerInputs();
    const bool sensorFaultActive = !currentInputs.hasPrimaryTemp;

    if (state == OutputState::On && sensorFaultActive) {
        LOG_WARN(
            "[app] ignoring output-on command during sensor fault target=%s reason=%s\r\n",
            target.c_str(),
            currentInputs.faultReason.isEmpty() ? "sensor fault active" : currentInputs.faultReason.c_str());
        return;
    }

    if (isOtaLockoutActive()) {
        LOG_WARN(
            "[app] ignoring output command during OTA reboot pending target=%s state=%s\r\n",
            target.c_str(),
            state == OutputState::On ? "on" : "off");
        return;
    }

    LOG_DEBUG(
        "[app] output command target=%s state=%s\r\n",
        target.c_str(),
        state == OutputState::On ? "on" : "off");

    const bool overrideWasActive = manualOutputOverrideActive_;
    const uint32_t previousOverrideUntilMs = manualOutputOverrideUntilMs_;
    bool changed = false;
    if (target == "heating") {
        changed = outputs_.setHeating(state);
    } else if (target == "cooling") {
        changed = outputs_.setCooling(state);
    } else if (target == "all") {
        changed = outputs_.setHeating(state) || outputs_.setCooling(state);
    } else {
        LOG_WARN("[app] ignoring output command with unsupported target=%s\r\n", target.c_str());
        return;
    }

    manualOutputOverrideActive_ = true;
    manualOutputOverrideUntilMs_ = millis() + kManualOutputOverrideDurationMs;
    outputs_.refreshStates();
    LOG_DEBUG(
        "[app] output command result changed=%s heating=%s cooling=%s manual_override_until_ms=%lu\r\n",
        changed ? "true" : "false",
        outputs_.describeHeating().c_str(),
        outputs_.describeCooling().c_str(),
        static_cast<unsigned long>(manualOutputOverrideUntilMs_));
    if (!overrideWasActive || previousOverrideUntilMs != manualOutputOverrideUntilMs_ || changed) {
        mqtt_.publishState(config_, outputs_, localUi_, buildTelemetrySnapshot());
    }
}

void App::handleProfileCommand(const String& command, const String& stepId) {
    if (manualOutputOverrideActive_) {
        clearManualOutputOverride("profile command");
    }
    if (fermentationConfig_.mode != "profile" || fermentationConfig_.profile.stepCount == 0) {
        return;
    }

    fermentationConfigRollback_ = fermentationConfig_;
    profileRuntimeRollback_ = profileRuntime_;
    bool changed = false;
    const uint32_t nowMs = millis();

    if (command == "profile_pause") {
        if (!profileRuntime_.paused) {
            freezeProfileTimers(nowMs);
            profileRuntime_.paused = true;
            profileRuntime_.phase = "paused";
            changed = true;
        }
    } else if (command == "profile_resume") {
        if (profileRuntime_.paused) {
            resumeProfileTimers(nowMs);
            profileRuntime_.paused = false;
            if (profileRuntime_.waitingForManualRelease) {
                profileRuntime_.phase = "waiting_manual_release";
            } else {
                profileRuntime_.phase = "holding";
            }
            changed = true;
        }
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

    if (fermentationConfig_.mode == "profile" && profileRuntime_.active) {
        updateProfileRuntime(nowMs, true);
        persistProfileRuntime(nowMs, true);
    } else {
        clearPersistedProfileRuntime();
    }
    mqtt_.publishState(config_, outputs_, localUi_, buildTelemetrySnapshot());
}

void App::clearManualOutputOverride(const char* reason) {
    if (!manualOutputOverrideActive_ && manualOutputOverrideUntilMs_ == 0) {
        return;
    }

    manualOutputOverrideActive_ = false;
    manualOutputOverrideUntilMs_ = 0;
    controller_.reset();
    LOG_INFO("[app] manual output override cleared reason=%s\r\n", reason ? reason : "unspecified");
}

void App::handleOtaCommand(const String& command, const String& channel) {
    LOG_DEBUG("[app] ota command=%s channel=%s\r\n", command.c_str(), channel.c_str());
    pendingOtaCommand_ = command;
    pendingOtaChannel_ = channel;
}

void App::processPendingOtaCommand() {
    if (pendingOtaCommand_.isEmpty()) {
        return;
    }

    const String command = pendingOtaCommand_;
    const String channel = pendingOtaChannel_;
    pendingOtaCommand_ = "";
    pendingOtaChannel_ = "";

    if (command == "check_update") {
        const OtaManager::CheckResult check = ota_.checkForUpdate(config_, channel);
        if (mqtt_.isConnected()) {
            mqtt_.publishState(config_, outputs_, localUi_, buildTelemetrySnapshot());
            mqtt_.publishEvent(
                config_,
                "ota_check_completed",
                check.success ? (check.updateAvailable ? "update_available" : "no_update") : "error",
                check.message.c_str(),
                buildTelemetrySnapshot());
        }
        return;
    }

    if (command != "start_update") {
        return;
    }

    const bool heatingChanged = outputs_.setHeating(OutputState::Off);
    const bool coolingChanged = outputs_.setCooling(OutputState::Off);
    outputs_.refreshStates();
    if (heatingChanged || coolingChanged) {
        mqtt_.publishState(config_, outputs_, localUi_, buildTelemetrySnapshot());
    }

    const OtaManager::InstallResult install = ota_.startUpdate(config_, channel);
    if (mqtt_.isConnected()) {
        mqtt_.publishState(config_, outputs_, localUi_, buildTelemetrySnapshot());
        mqtt_.publishEvent(
            config_,
            "ota_update_completed",
            install.success ? (ota_.status() == "rebooting" ? "ok" : "no_update") : "error",
            install.message.c_str(),
            buildTelemetrySnapshot());
    }
    if (install.success && ota_.status() == "rebooting") {
        otaShutdownPending_ = true;
        otaRestartAtMs_ = millis() + 1500UL;
    } else {
        otaShutdownPending_ = false;
    }
}

bool App::isOtaLockoutActive() const {
    return otaShutdownPending_ || otaRestartAtMs_ != 0;
}

MqttManager::TelemetrySnapshot App::buildTelemetrySnapshot() const {
    MqttManager::TelemetrySnapshot telemetry;
    const ControllerEngine::Status& controllerStatus = controller_.status();
    const SensorManager::Reading& beerProbe = sensors_.beerProbe();
    const SensorManager::Reading& chamberProbe = sensors_.chamberProbe();
    const uint32_t nowMs = millis();
    const bool beerProbeStale = isSensorStale(beerProbe, nowMs);
    const bool chamberProbeStale = isSensorStale(chamberProbe, nowMs);
    telemetry.hasSetpoint = true;
    telemetry.setpointC = fermentationConfig_.thermostat.setpointC;
    telemetry.hysteresisC = fermentationConfig_.thermostat.hysteresisC;
    telemetry.coolingDelaySeconds = fermentationConfig_.thermostat.coolingDelaySeconds;
    telemetry.heatingDelaySeconds = fermentationConfig_.thermostat.heatingDelaySeconds;
    telemetry.mode = fermentationConfig_.mode;
    telemetry.activeConfigVersion = fermentationConfig_.version;
    telemetry.controllerState = manualOutputOverrideActive_
        ? "manual_override"
        : ControllerEngine::stateName(controllerStatus.state);
    telemetry.controllerReason = manualOutputOverrideActive_
        ? "manual output override active"
        : controllerStatus.reason;
    telemetry.automaticControlActive = manualOutputOverrideActive_ ? false : controllerStatus.automaticControlActive;
    telemetry.secondarySensorEnabled = fermentationConfig_.sensors.secondaryEnabled;
    telemetry.controlSensor = fermentationConfig_.sensors.controlSensor;
    if (beerProbe.valid && !beerProbeStale) {
        telemetry.hasPrimaryTemp = true;
        telemetry.primaryTempC = beerProbe.tempC + fermentationConfig_.sensors.primaryOffsetC;
    }
    telemetry.beerProbePresent = beerProbe.present;
    telemetry.beerProbeValid = beerProbe.valid;
    telemetry.beerProbeStale = beerProbeStale;
    if (beerProbe.present) {
        telemetry.beerProbeRom = sensors_.beerProbeRom();
    }
    if (chamberProbe.valid && !chamberProbeStale) {
        telemetry.hasSecondaryTemp = true;
        telemetry.secondaryTempC = chamberProbe.tempC + fermentationConfig_.sensors.secondaryOffsetC;
    }
    telemetry.chamberProbePresent = chamberProbe.present;
    telemetry.chamberProbeValid = chamberProbe.valid;
    telemetry.chamberProbeStale = chamberProbeStale;
    if (chamberProbe.present) {
        telemetry.chamberProbeRom = sensors_.chamberProbeRom();
    }
    telemetry.fault = controllerStatus.state == ControllerEngine::State::Fault ? controllerStatus.reason : String();
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
        const uint32_t stepElapsedSeconds = currentProfileStepElapsedSeconds(nowMs);
        const uint32_t holdElapsedSeconds = currentProfileHoldElapsedSeconds(nowMs);
        telemetry.stepStartedAtSeconds =
            stepElapsedSeconds > (nowMs / 1000UL) ? 0 : (nowMs / 1000UL) - stepElapsedSeconds;
        telemetry.stepHoldStartedAtSeconds =
            (profileRuntime_.stepHoldStartedMs == 0 && profileRuntime_.holdBaseElapsedSeconds == 0)
            ? 0
            : (holdElapsedSeconds > (nowMs / 1000UL) ? 0 : (nowMs / 1000UL) - holdElapsedSeconds);
    }
    telemetry.otaStatus = ota_.status();
    telemetry.otaMessage = ota_.message();
    telemetry.otaChannel = config_.ota.channel;
    telemetry.otaTargetVersion = ota_.targetVersion();
    telemetry.otaUpdateAvailable = ota_.status() == "update_available";
    telemetry.otaProgressPercent = ota_.status() == "rebooting" ? 100 : 0;
    telemetry.otaRebootPending = isOtaLockoutActive();
    return telemetry;
}

ControllerEngine::Inputs App::buildControllerInputs() const {
    ControllerEngine::Inputs inputs;
    const SensorManager::Reading& beerProbe = sensors_.beerProbe();
    const SensorManager::Reading& chamberProbe = sensors_.chamberProbe();
    inputs.nowMs = millis();
    const bool beerProbeStale = isSensorStale(beerProbe, inputs.nowMs);
    const bool chamberProbeStale = isSensorStale(chamberProbe, inputs.nowMs);
    const bool beerProbeValid = beerProbe.valid && !beerProbeStale;
    const bool chamberProbeValid = chamberProbe.valid && !chamberProbeStale;
    const bool useSecondaryAsControl =
        fermentationConfig_.sensors.secondaryEnabled && fermentationConfig_.sensors.controlSensor == "secondary";

    if (useSecondaryAsControl) {
        inputs.hasPrimaryTemp = chamberProbeValid;
        inputs.primaryTempC = chamberProbe.tempC + fermentationConfig_.sensors.secondaryOffsetC;
        inputs.hasSecondaryTemp = beerProbeValid;
        inputs.secondaryTempC = beerProbe.tempC + fermentationConfig_.sensors.primaryOffsetC;
    } else {
        inputs.hasPrimaryTemp = beerProbeValid;
        inputs.primaryTempC = beerProbe.tempC + fermentationConfig_.sensors.primaryOffsetC;
        inputs.hasSecondaryTemp = chamberProbeValid;
        inputs.secondaryTempC = chamberProbe.tempC + fermentationConfig_.sensors.secondaryOffsetC;
    }
    if (!inputs.hasPrimaryTemp) {
        inputs.faultReason = sensorFaultReason(
            useSecondaryAsControl ? "chamber" : "beer",
            useSecondaryAsControl ? chamberProbe : beerProbe,
            useSecondaryAsControl ? chamberProbeStale : beerProbeStale);
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
    lastProfileRuntimePersistMs_ = 0;
}

void App::initializeProfileRuntime() {
    resetProfileRuntime();

    if (fermentationConfig_.mode != "profile" || fermentationConfig_.profile.stepCount == 0) {
        clearPersistedProfileRuntime();
        return;
    }

    const uint32_t nowMs = millis();
    if (restoreProfileRuntime(nowMs)) {
        updateProfileRuntime(nowMs, true);
        return;
    }

    activateProfileStep(0, true);
    updateProfileRuntime(nowMs, true);
}

bool App::activateProfileStep(uint8_t stepIndex, bool treatAsFreshStep) {
    if (stepIndex >= fermentationConfig_.profile.stepCount) {
        return false;
    }

    const ProfileStepConfig& step = fermentationConfig_.profile.steps[stepIndex];
    const float previousTargetC =
        stepIndex == 0 ? step.targetC : fermentationConfig_.profile.steps[stepIndex - 1].targetC;
    const bool hasRamp = stepIndex > 0 && step.rampDurationSeconds > 0;
    profileRuntime_.active = true;
    profileRuntime_.activeProfileId = fermentationConfig_.profile.id;
    profileRuntime_.activeStepId = step.id;
    profileRuntime_.activeStepIndex = stepIndex;
    profileRuntime_.paused = false;
    profileRuntime_.waitingForManualRelease = false;
    profileRuntime_.holdTimingActive = !hasRamp;
    profileRuntime_.phase = hasRamp
        ? "ramping"
        : (step.advancePolicy == "manual_release" ? "waiting_manual_release" : "holding");
    profileRuntime_.effectiveTargetC = hasRamp ? previousTargetC : step.targetC;
    if (treatAsFreshStep) {
        const uint32_t nowMs = millis();
        profileRuntime_.stepStartedMs = nowMs;
        profileRuntime_.stepHoldStartedMs = hasRamp ? 0 : nowMs;
        profileRuntime_.stepBaseElapsedSeconds = 0;
        profileRuntime_.holdBaseElapsedSeconds = 0;
    }

    fermentationConfig_.mode = "profile";
    fermentationConfig_.thermostat.setpointC = profileRuntime_.effectiveTargetC;
    return true;
}

bool App::updateProfileRuntime(uint32_t nowMs, bool allowProgress) {
    if (fermentationConfig_.mode != "profile" || fermentationConfig_.profile.stepCount == 0 || !profileRuntime_.active) {
        return false;
    }

    if (
        profileRuntime_.activeProfileId != fermentationConfig_.profile.id || profileRuntime_.activeStepIndex < 0
        || profileRuntime_.activeStepIndex >= fermentationConfig_.profile.stepCount
        || fermentationConfig_.profile.steps[profileRuntime_.activeStepIndex].id != profileRuntime_.activeStepId) {
        resetProfileRuntime();
        clearPersistedProfileRuntime();
        return true;
    }

    const ProfileStepConfig& step = fermentationConfig_.profile.steps[profileRuntime_.activeStepIndex];
    const float previousTargetC = profileRuntime_.activeStepIndex == 0
        ? step.targetC
        : fermentationConfig_.profile.steps[profileRuntime_.activeStepIndex - 1].targetC;
    bool stateChanged = false;

    if (profileRuntime_.phase == "completed") {
        fermentationConfig_.thermostat.setpointC = step.targetC;
        persistProfileRuntime(nowMs, false);
        return false;
    }

    if (!allowProgress) {
        bool stateChanged = false;
        if (profileRuntime_.phase != "faulted") {
            freezeProfileTimers(nowMs);
            profileRuntime_.phase = "faulted";
            stateChanged = true;
        }
        if (stateChanged) {
            persistProfileRuntime(nowMs, true);
        } else {
            persistProfileRuntime(nowMs, false);
        }
        return stateChanged;
    }

    if (profileRuntime_.phase == "faulted") {
        resumeProfileTimers(nowMs);
    }

    if (profileRuntime_.paused) {
        if (profileRuntime_.phase != "paused") {
            profileRuntime_.phase = "paused";
            persistProfileRuntime(nowMs, true);
            fermentationConfig_.thermostat.setpointC = profileRuntime_.effectiveTargetC;
            return true;
        }
        fermentationConfig_.thermostat.setpointC = profileRuntime_.effectiveTargetC;
        persistProfileRuntime(nowMs, false);
        return false;
    }

    const uint32_t stepElapsedSeconds = currentProfileStepElapsedSeconds(nowMs);
    if (profileRuntime_.activeStepIndex > 0 && step.rampDurationSeconds > 0 && stepElapsedSeconds < step.rampDurationSeconds) {
        const float rampProgress = static_cast<float>(stepElapsedSeconds) / static_cast<float>(step.rampDurationSeconds);
        if (profileRuntime_.phase != "ramping") {
            profileRuntime_.phase = "ramping";
            stateChanged = true;
        }
        if (profileRuntime_.waitingForManualRelease) {
            profileRuntime_.waitingForManualRelease = false;
            stateChanged = true;
        }
        if (profileRuntime_.holdTimingActive) {
            profileRuntime_.holdTimingActive = false;
            stateChanged = true;
        }
        profileRuntime_.effectiveTargetC = previousTargetC + ((step.targetC - previousTargetC) * rampProgress);
        if (profileRuntime_.stepHoldStartedMs != 0 || profileRuntime_.holdBaseElapsedSeconds != 0) {
            profileRuntime_.stepHoldStartedMs = 0;
            profileRuntime_.holdBaseElapsedSeconds = 0;
            stateChanged = true;
        }
        fermentationConfig_.thermostat.setpointC = profileRuntime_.effectiveTargetC;
        persistProfileRuntime(nowMs, false);
        return stateChanged;
    }

    fermentationConfig_.thermostat.setpointC = step.targetC;
    profileRuntime_.effectiveTargetC = step.targetC;
    if (!profileRuntime_.holdTimingActive) {
        profileRuntime_.holdTimingActive = true;
        stateChanged = true;
    }
    if (profileRuntime_.stepHoldStartedMs == 0) {
        profileRuntime_.stepHoldStartedMs = nowMs;
        profileRuntime_.holdBaseElapsedSeconds = 0;
        stateChanged = true;
    }

    const bool waitingForManualRelease = step.advancePolicy == "manual_release";
    const String nextPhase = waitingForManualRelease ? "waiting_manual_release" : "holding";
    if (profileRuntime_.phase != nextPhase) {
        profileRuntime_.phase = nextPhase;
        stateChanged = true;
    }
    if (profileRuntime_.waitingForManualRelease != waitingForManualRelease) {
        profileRuntime_.waitingForManualRelease = waitingForManualRelease;
        stateChanged = true;
    }

    if (!waitingForManualRelease) {
        const uint32_t holdElapsedSeconds = currentProfileHoldElapsedSeconds(nowMs);
        if (holdElapsedSeconds >= step.holdDurationSeconds) {
            if ((profileRuntime_.activeStepIndex + 1) < fermentationConfig_.profile.stepCount) {
                const bool advanced = activateProfileStep(static_cast<uint8_t>(profileRuntime_.activeStepIndex + 1), true);
                if (advanced) {
                    persistProfileRuntime(nowMs, true);
                }
                return advanced;
            }

            profileRuntime_.phase = "completed";
            profileRuntime_.waitingForManualRelease = false;
            stateChanged = true;
        }
    }

    if (stateChanged) {
        persistProfileRuntime(nowMs, true);
    } else {
        persistProfileRuntime(nowMs, false);
    }
    return stateChanged;
}

bool App::restoreProfileRuntime(uint32_t nowMs) {
    ProfileRuntimeState restored;
    if (!configStore_.loadProfileRuntime(fermentationConfig_.version, restored)) {
        return false;
    }

    if (
        restored.activeProfileId != fermentationConfig_.profile.id || restored.activeStepIndex < 0
        || restored.activeStepIndex >= fermentationConfig_.profile.stepCount
        || fermentationConfig_.profile.steps[restored.activeStepIndex].id != restored.activeStepId) {
        clearPersistedProfileRuntime();
        return false;
    }

    profileRuntime_ = restored;
    profileRuntime_.stepStartedMs = nowMs;
    profileRuntime_.stepHoldStartedMs = profileRuntime_.holdTimingActive ? nowMs : 0;
    fermentationConfig_.thermostat.setpointC = profileRuntime_.effectiveTargetC;
    return true;
}

bool App::persistProfileRuntime(uint32_t nowMs, bool force) {
    if (fermentationConfig_.mode != "profile" || !profileRuntime_.active) {
        return false;
    }

    if (!force && nowMs - lastProfileRuntimePersistMs_ < kProfileRuntimePersistIntervalMs) {
        return false;
    }

    if (!configStore_.saveProfileRuntime(fermentationConfig_.version, profileRuntime_, nowMs)) {
        return false;
    }

    lastProfileRuntimePersistMs_ = nowMs;
    return true;
}

void App::clearPersistedProfileRuntime() {
    configStore_.clearProfileRuntime();
    lastProfileRuntimePersistMs_ = 0;
}

uint32_t App::currentProfileStepElapsedSeconds(uint32_t nowMs) const {
    uint32_t elapsedSeconds = profileRuntime_.stepBaseElapsedSeconds;
    if (
        !profileRuntime_.paused && profileRuntime_.phase != "faulted" && profileRuntime_.phase != "completed"
        && profileRuntime_.stepStartedMs != 0) {
        elapsedSeconds += (nowMs - profileRuntime_.stepStartedMs) / 1000UL;
    }
    return elapsedSeconds;
}

uint32_t App::currentProfileHoldElapsedSeconds(uint32_t nowMs) const {
    uint32_t elapsedSeconds = profileRuntime_.holdBaseElapsedSeconds;
    if (
        !profileRuntime_.paused && profileRuntime_.phase != "faulted" && profileRuntime_.phase != "completed"
        && profileRuntime_.stepHoldStartedMs != 0) {
        elapsedSeconds += (nowMs - profileRuntime_.stepHoldStartedMs) / 1000UL;
    }
    return elapsedSeconds;
}

void App::freezeProfileTimers(uint32_t nowMs) {
    profileRuntime_.stepBaseElapsedSeconds = currentProfileStepElapsedSeconds(nowMs);
    profileRuntime_.stepStartedMs = nowMs;
    profileRuntime_.holdBaseElapsedSeconds = currentProfileHoldElapsedSeconds(nowMs);
    if (profileRuntime_.stepHoldStartedMs != 0) {
        profileRuntime_.stepHoldStartedMs = nowMs;
    }
}

void App::resumeProfileTimers(uint32_t nowMs) {
    profileRuntime_.stepStartedMs = nowMs;
    if (profileRuntime_.stepHoldStartedMs != 0 || profileRuntime_.holdBaseElapsedSeconds > 0) {
        profileRuntime_.stepHoldStartedMs = nowMs;
    }
}

void App::runOutputDiscovery() {
    LOG_DEBUG(
        "[app] running output discovery wifi=%s mqtt=%s local_ip=%s mask=%s\r\n",
        WiFi.status() == WL_CONNECTED ? "connected" : "disconnected",
        mqtt_.isConnected() ? "connected" : "disconnected",
        WiFi.localIP().toString().c_str(),
        WiFi.subnetMask().toString().c_str());

    bool found = false;
    LOG_DEBUG_MSG("[app] starting Kasa discovery");
    found = kasaDiscovery_.discover(
                [this](const String& payload) { mqtt_.publishOutputDiscovery(config_, payload); })
        || found;
    LOG_DEBUG_MSG("[app] Kasa discovery finished");
    LOG_DEBUG_MSG("[app] starting Shelly discovery");
    found = shellyDiscovery_.discover(
                [this](const String& payload) { mqtt_.publishOutputDiscovery(config_, payload); })
        || found;
    LOG_DEBUG_MSG("[app] Shelly discovery finished");

    if (!found) {
        LOG_DEBUG_MSG("[app] output discovery returned no devices");
    } else {
        LOG_DEBUG_MSG("[app] output discovery published at least one device");
    }
}

void App::startProvisioningMode(const char* reason) {
    if (provisioningMode_ && provisioning_.isActive()) {
        return;
    }

    Serial.printf("[prov] starting provisioning mode reason=%s\r\n", reason);
    wifiConnectStartedMs_ = 0;
    provisioningMode_ = provisioning_.begin(
        config_,
        [this](const SystemConfig& updatedConfig) {
            config_ = updatedConfig;
            return configStore_.save(config_);
        });
}

void App::ensureWifiConnected() {
    if (config_.wifi.ssid.isEmpty()) {
        wifiConnectStartedMs_ = 0;
        return;
    }

    if (WiFi.status() == WL_CONNECTED) {
        wifiConnectStartedMs_ = 0;
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

    if (provisioningMode_) {
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

    config.heatingOutput.driver = OutputDriverType::None;
    config.heatingOutput.host = "";
    config.heatingOutput.port = 9999;
    config.heatingOutput.alias = "heating-plug";
    config.heatingOutput.pollIntervalSeconds = 30;

    config.coolingOutput.driver = OutputDriverType::None;
    config.coolingOutput.host = "";
    config.coolingOutput.port = 9999;
    config.coolingOutput.alias = "cooling-plug";
    config.coolingOutput.pollIntervalSeconds = 30;

    config.ota.enabled = true;
    config.ota.channel = "stable";
    config.ota.checkStrategy = "manual";
    config.ota.checkIntervalSeconds = 86400;
    config.ota.manifestUrl = "";
    config.ota.caCertFingerprint = "";
    config.ota.allowHttp = false;

    return config;
}
