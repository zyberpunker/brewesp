#include "config/ConfigStore.h"

#include <ArduinoJson.h>
#include <Preferences.h>

namespace {
const char* kNamespace = "brewesp";
constexpr size_t kMaxStoredProfileBytes = 3072;
constexpr size_t kMaxStoredProfileRuntimeBytes = 768;

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

String outputDriverToString(OutputDriverType type) {
    switch (type) {
        case OutputDriverType::Gpio:
            return "gpio";
        case OutputDriverType::ShellyHttpRpc:
            return "shelly_http_rpc";
        case OutputDriverType::KasaLocal:
            return "kasa_local";
        case OutputDriverType::GenericMqttRelay:
            return "generic_mqtt_relay";
        case OutputDriverType::None:
        default:
            return "none";
    }
}
}

bool ConfigStore::load(SystemConfig& config) {
    Preferences prefs;
    if (!prefs.begin(kNamespace, true)) {
        if (!prefs.begin(kNamespace, false)) {
            return false;
        }
    }

    config.deviceId = prefs.getString("device_id", config.deviceId);
    config.wifi.ssid = prefs.getString("wifi_ssid", "");
    config.wifi.password = prefs.getString("wifi_pass", "");
    config.wifi.recoveryAp.ssid = prefs.getString("ap_ssid", "");
    config.wifi.recoveryAp.password =
        prefs.getString("ap_pass", config.wifi.recoveryAp.password);
    config.wifi.recoveryAp.ip = prefs.getString("ap_ip", config.wifi.recoveryAp.ip);
    config.wifi.recoveryAp.enabled = prefs.getBool("ap_en", true);
    config.wifi.recoveryAp.autoStartWhenUnprovisioned = prefs.getBool("ap_auto", true);
    config.wifi.recoveryAp.startAfterWifiFailureSeconds =
        prefs.getUInt("ap_fail_s", config.wifi.recoveryAp.startAfterWifiFailureSeconds);

    config.mqtt.host = prefs.getString("mqtt_host", "");
    config.mqtt.port = prefs.getUInt("mqtt_port", config.mqtt.port);
    config.mqtt.clientId = prefs.getString("mqtt_id", config.deviceId);
    config.mqtt.username = prefs.getString("mqtt_user", "");
    config.mqtt.password = prefs.getString("mqtt_pass", "");
    config.mqtt.topicPrefix = prefs.getString("mqtt_pref", config.mqtt.topicPrefix);

    config.heartbeat.intervalSeconds =
        prefs.getUInt("hb_int", config.heartbeat.intervalSeconds);
    config.sensors.oneWireGpio = prefs.getUChar("sns_gpio", config.sensors.oneWireGpio);
    config.sensors.pollIntervalSeconds =
        prefs.getUInt("sns_poll", config.sensors.pollIntervalSeconds);
    config.sensors.beerProbeRom = prefs.getString("sns_beer", config.sensors.beerProbeRom);
    config.sensors.chamberProbeRom =
        prefs.getString("sns_chmb", config.sensors.chamberProbeRom);

    config.localUi.enabled = prefs.getBool("ui_en", config.localUi.enabled);
    config.localUi.headlessAllowed = prefs.getBool("ui_headless", true);

    config.display.enabled = prefs.getBool("disp_en", config.display.enabled);
    config.display.driver = prefs.getString("disp_drv", config.display.driver);
    config.display.i2cAddress = prefs.getString("disp_i2c", config.display.i2cAddress);

    config.buttons.enabled = prefs.getBool("btn_en", config.buttons.enabled);
    config.buttons.upGpio = prefs.getUChar("btn_up", config.buttons.upGpio);
    config.buttons.downGpio = prefs.getUChar("btn_dn", config.buttons.downGpio);
    config.buttons.selectGpio = prefs.getUChar("btn_sel", config.buttons.selectGpio);
    config.buttons.backGpio = prefs.getUChar("btn_back", config.buttons.backGpio);

    config.heatingOutput.driver = outputDriverFromString(
        prefs.getString("heat_drv", outputDriverToString(config.heatingOutput.driver)));
    config.heatingOutput.pin = prefs.getChar("heat_pin", config.heatingOutput.pin);
    config.heatingOutput.host = prefs.getString("heat_host", config.heatingOutput.host);
    config.heatingOutput.port = prefs.getUInt("heat_port", config.heatingOutput.port);
    config.heatingOutput.alias = prefs.getString("heat_alias", config.heatingOutput.alias);

    config.coolingOutput.driver = outputDriverFromString(
        prefs.getString("cool_drv", outputDriverToString(config.coolingOutput.driver)));
    config.coolingOutput.pin = prefs.getChar("cool_pin", config.coolingOutput.pin);
    config.coolingOutput.host = prefs.getString("cool_host", config.coolingOutput.host);
    config.coolingOutput.port = prefs.getUInt("cool_port", config.coolingOutput.port);
    config.coolingOutput.alias = prefs.getString("cool_alias", config.coolingOutput.alias);
    config.ota.enabled = prefs.getBool("ota_en", config.ota.enabled);
    config.ota.channel = prefs.getString("ota_chan", config.ota.channel);
    config.ota.checkStrategy = prefs.getString("ota_chk", config.ota.checkStrategy);
    config.ota.checkIntervalSeconds =
        prefs.getUInt("ota_int", config.ota.checkIntervalSeconds);
    config.ota.manifestUrl = prefs.getString("ota_url", config.ota.manifestUrl);
    config.ota.caCertFingerprint =
        prefs.getString("ota_fp", config.ota.caCertFingerprint);
    config.ota.allowHttp = prefs.getBool("ota_http", config.ota.allowHttp);

    prefs.end();
    return !config.wifi.ssid.isEmpty();
}

bool ConfigStore::save(const SystemConfig& config) {
    Preferences prefs;
    if (!prefs.begin(kNamespace, false)) {
        return false;
    }

    prefs.putString("device_id", config.deviceId);
    prefs.putString("wifi_ssid", config.wifi.ssid);
    prefs.putString("wifi_pass", config.wifi.password);
    prefs.putString("ap_ssid", config.wifi.recoveryAp.ssid);
    prefs.putString("ap_pass", config.wifi.recoveryAp.password);
    prefs.putString("ap_ip", config.wifi.recoveryAp.ip);
    prefs.putBool("ap_en", config.wifi.recoveryAp.enabled);
    prefs.putBool("ap_auto", config.wifi.recoveryAp.autoStartWhenUnprovisioned);
    prefs.putUInt("ap_fail_s", config.wifi.recoveryAp.startAfterWifiFailureSeconds);

    prefs.putString("mqtt_host", config.mqtt.host);
    prefs.putUInt("mqtt_port", config.mqtt.port);
    prefs.putString("mqtt_id", config.mqtt.clientId);
    prefs.putString("mqtt_user", config.mqtt.username);
    prefs.putString("mqtt_pass", config.mqtt.password);
    prefs.putString("mqtt_pref", config.mqtt.topicPrefix);

    prefs.putUInt("hb_int", config.heartbeat.intervalSeconds);
    prefs.putUChar("sns_gpio", config.sensors.oneWireGpio);
    prefs.putUInt("sns_poll", config.sensors.pollIntervalSeconds);
    prefs.putString("sns_beer", config.sensors.beerProbeRom);
    prefs.putString("sns_chmb", config.sensors.chamberProbeRom);

    prefs.putBool("ui_en", config.localUi.enabled);
    prefs.putBool("ui_headless", config.localUi.headlessAllowed);

    prefs.putBool("disp_en", config.display.enabled);
    prefs.putString("disp_drv", config.display.driver);
    prefs.putString("disp_i2c", config.display.i2cAddress);

    prefs.putBool("btn_en", config.buttons.enabled);
    prefs.putUChar("btn_up", config.buttons.upGpio);
    prefs.putUChar("btn_dn", config.buttons.downGpio);
    prefs.putUChar("btn_sel", config.buttons.selectGpio);
    prefs.putUChar("btn_back", config.buttons.backGpio);

    prefs.putString("heat_drv", outputDriverToString(config.heatingOutput.driver));
    prefs.putChar("heat_pin", config.heatingOutput.pin);
    prefs.putString("heat_host", config.heatingOutput.host);
    prefs.putUInt("heat_port", config.heatingOutput.port);
    prefs.putString("heat_alias", config.heatingOutput.alias);

    prefs.putString("cool_drv", outputDriverToString(config.coolingOutput.driver));
    prefs.putChar("cool_pin", config.coolingOutput.pin);
    prefs.putString("cool_host", config.coolingOutput.host);
    prefs.putUInt("cool_port", config.coolingOutput.port);
    prefs.putString("cool_alias", config.coolingOutput.alias);
    prefs.putBool("ota_en", config.ota.enabled);
    prefs.putString("ota_chan", config.ota.channel);
    prefs.putString("ota_chk", config.ota.checkStrategy);
    prefs.putUInt("ota_int", config.ota.checkIntervalSeconds);
    prefs.putString("ota_url", config.ota.manifestUrl);
    prefs.putString("ota_fp", config.ota.caCertFingerprint);
    prefs.putBool("ota_http", config.ota.allowHttp);

    prefs.end();
    return true;
}

bool ConfigStore::loadFermentationConfig(FermentationConfig& config) {
    Preferences prefs;
    if (!prefs.begin(kNamespace, true)) {
        if (!prefs.begin(kNamespace, false)) {
            return false;
        }
    }

    config.schemaVersion = prefs.getUInt("fcfg_schema", config.schemaVersion);
    config.version = prefs.getUInt("fcfg_ver", config.version);
    config.deviceId = prefs.getString("fcfg_dev", config.deviceId);
    config.name = prefs.getString("fcfg_name", config.name);
    config.mode = prefs.getString("fcfg_mode", config.mode);
    config.thermostat.setpointC = prefs.getFloat("fcfg_setpt", config.thermostat.setpointC);
    config.thermostat.hysteresisC =
        prefs.getFloat("fcfg_hyst", config.thermostat.hysteresisC);
    config.thermostat.coolingDelaySeconds =
        prefs.getUInt("fcfg_coold", config.thermostat.coolingDelaySeconds);
    config.thermostat.heatingDelaySeconds =
        prefs.getUInt("fcfg_heatd", config.thermostat.heatingDelaySeconds);
    config.sensors.primaryOffsetC =
        prefs.getFloat("fcfg_poff", config.sensors.primaryOffsetC);
    config.sensors.secondaryEnabled =
        prefs.getBool("fcfg_s2en", config.sensors.secondaryEnabled);
    config.sensors.secondaryOffsetC =
        prefs.getFloat("fcfg_s2off", config.sensors.secondaryOffsetC);
    config.sensors.secondaryLimitHysteresisC =
        prefs.getFloat("fcfg_s2hy", config.sensors.secondaryLimitHysteresisC);
    config.sensors.controlSensor =
        prefs.getString("fcfg_ctrl", config.sensors.controlSensor);

    if (prefs.isKey("fcfg_prof")) {
        const String profileJson = prefs.getString("fcfg_prof", "");
        DynamicJsonDocument doc(4096);
        if (deserializeJson(doc, profileJson) == DeserializationError::Ok) {
            config.profile.id = String(static_cast<const char*>(doc["id"] | ""));
            config.profile.stepCount = 0;
            JsonArray steps = doc["steps"].as<JsonArray>();
            for (JsonObject step : steps) {
                if (config.profile.stepCount >= kMaxProfileSteps) {
                    break;
                }

                ProfileStepConfig& entry = config.profile.steps[config.profile.stepCount++];
                entry.id = String(static_cast<const char*>(step["id"] | ""));
                entry.label = String(static_cast<const char*>(step["label"] | ""));
                entry.targetC = step["target_c"] | entry.targetC;
                entry.holdDurationSeconds = step["hold_duration_s"] | entry.holdDurationSeconds;
                entry.rampDurationSeconds = step["ramp_duration_s"] | entry.rampDurationSeconds;
                entry.advancePolicy = String(static_cast<const char*>(step["advance_policy"] | "auto"));
            }
        } else if (config.mode == "profile") {
            config.mode = "thermostat";
        }
    }

    prefs.end();
    return true;
}

bool ConfigStore::saveFermentationConfig(const FermentationConfig& config) {
    Preferences prefs;
    if (!prefs.begin(kNamespace, false)) {
        return false;
    }

    prefs.putUInt("fcfg_schema", config.schemaVersion);
    prefs.putUInt("fcfg_ver", config.version);
    prefs.putString("fcfg_dev", config.deviceId);
    prefs.putString("fcfg_name", config.name);
    prefs.putString("fcfg_mode", config.mode);
    prefs.putFloat("fcfg_setpt", config.thermostat.setpointC);
    prefs.putFloat("fcfg_hyst", config.thermostat.hysteresisC);
    prefs.putUInt("fcfg_coold", config.thermostat.coolingDelaySeconds);
    prefs.putUInt("fcfg_heatd", config.thermostat.heatingDelaySeconds);
    prefs.putFloat("fcfg_poff", config.sensors.primaryOffsetC);
    prefs.putBool("fcfg_s2en", config.sensors.secondaryEnabled);
    prefs.putFloat("fcfg_s2off", config.sensors.secondaryOffsetC);
    prefs.putFloat("fcfg_s2hy", config.sensors.secondaryLimitHysteresisC);
    prefs.putString("fcfg_ctrl", config.sensors.controlSensor);

    DynamicJsonDocument doc(4096);
    doc["id"] = config.profile.id;
    JsonArray steps = doc.createNestedArray("steps");
    for (uint8_t i = 0; i < config.profile.stepCount; ++i) {
        const ProfileStepConfig& step = config.profile.steps[i];
        JsonObject item = steps.createNestedObject();
        item["id"] = step.id;
        item["label"] = step.label;
        item["target_c"] = step.targetC;
        item["hold_duration_s"] = step.holdDurationSeconds;
        if (step.rampDurationSeconds > 0) {
            item["ramp_duration_s"] = step.rampDurationSeconds;
        }
        item["advance_policy"] = step.advancePolicy;
    }

    if (doc.overflowed()) {
        prefs.end();
        return false;
    }

    String profileJson;
    const size_t serializedLength = serializeJson(doc, profileJson);
    if (serializedLength == 0 || serializedLength > kMaxStoredProfileBytes) {
        prefs.end();
        return false;
    }

    const size_t storedLength = prefs.putString("fcfg_prof", profileJson);
    if (storedLength == 0 && !profileJson.isEmpty()) {
        prefs.end();
        return false;
    }

    prefs.end();
    return true;
}

bool ConfigStore::loadProfileRuntime(uint32_t expectedConfigVersion, ProfileRuntimeState& runtime) {
    Preferences prefs;
    if (!prefs.begin(kNamespace, true)) {
        if (!prefs.begin(kNamespace, false)) {
            return false;
        }
    }

    const String runtimeJson = prefs.getString("frt_state", "");
    prefs.end();
    if (runtimeJson.isEmpty()) {
        return false;
    }

    DynamicJsonDocument doc(1024);
    if (deserializeJson(doc, runtimeJson) != DeserializationError::Ok) {
        return false;
    }

    if ((doc["config_version"] | 0U) != expectedConfigVersion) {
        return false;
    }

    ProfileRuntimeState loaded;
    loaded.active = doc["active"] | false;
    loaded.activeProfileId = String(static_cast<const char*>(doc["active_profile_id"] | ""));
    loaded.activeStepId = String(static_cast<const char*>(doc["active_step_id"] | ""));
    loaded.activeStepIndex = doc["active_step_index"] | -1;
    loaded.phase = String(static_cast<const char*>(doc["phase"] | "idle"));
    loaded.paused = doc["paused"] | false;
    loaded.waitingForManualRelease = doc["waiting_for_manual_release"] | false;
    loaded.holdTimingActive = doc["hold_timing_active"] | false;
    loaded.effectiveTargetC = doc["effective_target_c"] | 0.0f;
    loaded.stepBaseElapsedSeconds = doc["step_elapsed_s"] | 0UL;
    loaded.holdBaseElapsedSeconds = doc["hold_elapsed_s"] | 0UL;
    runtime = loaded;
    return loaded.active;
}

bool ConfigStore::saveProfileRuntime(uint32_t configVersion, const ProfileRuntimeState& runtime, uint32_t nowMs) {
    Preferences prefs;
    if (!prefs.begin(kNamespace, false)) {
        return false;
    }

    DynamicJsonDocument doc(1024);
    doc["config_version"] = configVersion;
    doc["active"] = runtime.active;
    doc["active_profile_id"] = runtime.activeProfileId;
    doc["active_step_id"] = runtime.activeStepId;
    doc["active_step_index"] = runtime.activeStepIndex;
    doc["phase"] = runtime.phase;
    doc["paused"] = runtime.paused;
    doc["waiting_for_manual_release"] = runtime.waitingForManualRelease;
    doc["hold_timing_active"] = runtime.holdTimingActive;
    doc["effective_target_c"] = runtime.effectiveTargetC;

    const bool timersRunning = !runtime.paused && runtime.phase != "faulted" && runtime.phase != "completed";
    uint32_t stepElapsedSeconds = runtime.stepBaseElapsedSeconds;
    if (timersRunning && runtime.stepStartedMs != 0 && static_cast<int32_t>(nowMs - runtime.stepStartedMs) >= 0) {
        stepElapsedSeconds += (nowMs - runtime.stepStartedMs) / 1000UL;
    }
    doc["step_elapsed_s"] = stepElapsedSeconds;

    uint32_t holdElapsedSeconds = runtime.holdBaseElapsedSeconds;
    if (timersRunning && runtime.stepHoldStartedMs != 0 && static_cast<int32_t>(nowMs - runtime.stepHoldStartedMs) >= 0) {
        holdElapsedSeconds += (nowMs - runtime.stepHoldStartedMs) / 1000UL;
    }
    doc["hold_elapsed_s"] = holdElapsedSeconds;

    if (doc.overflowed()) {
        prefs.end();
        return false;
    }

    String runtimeJson;
    const size_t serializedLength = serializeJson(doc, runtimeJson);
    if (serializedLength == 0 || serializedLength > kMaxStoredProfileRuntimeBytes) {
        prefs.end();
        return false;
    }

    const size_t storedLength = prefs.putString("frt_state", runtimeJson);
    prefs.end();
    return storedLength != 0 || runtimeJson.isEmpty();
}

bool ConfigStore::clearProfileRuntime() {
    Preferences prefs;
    if (!prefs.begin(kNamespace, false)) {
        return false;
    }

    const bool removed = prefs.remove("frt_state");
    prefs.end();
    return removed;
}
