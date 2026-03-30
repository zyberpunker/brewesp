#include "network/MqttManager.h"

#include <math.h>

#include <ArduinoJson.h>
#include <WiFi.h>

#include "config/FirmwareVersion.h"
#include "output/OutputManager.h"
#include "ui/LocalUiManager.h"

namespace {
constexpr uint32_t kTelemetryIntervalMs = 15000UL;

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
    client_.setBufferSize(1024);
    client_.setCallback(
        [this](char* topic, uint8_t* payload, unsigned int length) {
            handleMessage(currentConfig_, topic, payload, length);
        });
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
        return;
    }

    client_.loop();

    if (!client_.connected()) {
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

void MqttManager::setSystemConfigHandler(SystemConfigHandler handler) {
    systemConfigHandler_ = handler;
}

void MqttManager::setFermentationConfigHandler(FermentationConfigHandler handler) {
    fermentationConfigHandler_ = handler;
}

void MqttManager::setOutputCommandHandler(OutputCommandHandler handler) {
    outputCommandHandler_ = handler;
}

void MqttManager::setDiscoveryRequestHandler(DiscoveryRequestHandler handler) {
    discoveryRequestHandler_ = handler;
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
        Serial.printf("[mqtt] connect failed rc=%d\r\n", client_.state());
        return false;
    }

    Serial.printf("[mqtt] connected host=%s port=%u\r\n", config.mqtt.host.c_str(), config.mqtt.port);
    client_.subscribe(topicFor(config, "system_config").c_str());
    client_.subscribe(topicFor(config, "command").c_str());
    client_.subscribe(topicFor(config, "config/desired").c_str());
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
    Serial.printf("[mqtt] heartbeat published topic=%s\r\n", topicFor(config, "heartbeat").c_str());
}

void MqttManager::publishState(
    const SystemConfig& config,
    const OutputManager& outputs,
    const LocalUiManager& localUi,
    const TelemetrySnapshot& telemetry) {
    const String heatingDescription = outputs.describeHeating();
    const String coolingDescription = outputs.describeCooling();

    StaticJsonDocument<896> doc;
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
    doc["secondary_sensor_enabled"] = telemetry.secondarySensorEnabled;
    doc["control_sensor"] = telemetry.controlSensor;
    doc["beer_probe_present"] = telemetry.beerProbePresent;
    doc["beer_probe_valid"] = telemetry.beerProbeValid;
    doc["beer_probe_rom"] = telemetry.beerProbeRom;
    doc["chamber_probe_present"] = telemetry.chamberProbePresent;
    doc["chamber_probe_valid"] = telemetry.chamberProbeValid;
    doc["chamber_probe_rom"] = telemetry.chamberProbeRom;
    if (telemetry.hasSetpoint) {
        doc["setpoint_c"] = telemetry.setpointC;
    } else {
        doc["setpoint_c"] = nullptr;
    }
    doc["hysteresis_c"] = telemetry.hysteresisC;
    doc["cooling_delay_s"] = telemetry.coolingDelaySeconds;
    doc["heating_delay_s"] = telemetry.heatingDelaySeconds;
    if (!telemetry.otaTargetVersion.isEmpty()) {
        doc["ota_target_version"] = telemetry.otaTargetVersion;
    }
    if (!telemetry.otaMessage.isEmpty()) {
        doc["ota_message"] = telemetry.otaMessage;
    }

    char payload[896];
    serializeJson(doc, payload, sizeof(payload));
    client_.publish(topicFor(config, "state").c_str(), payload, true);
}

void MqttManager::publishTelemetry(
    const SystemConfig& config,
    const OutputManager& outputs,
    const TelemetrySnapshot& telemetry) {
    StaticJsonDocument<512> doc;
    doc["device_id"] = config.deviceId;
    doc["ts"] = millis() / 1000UL;
    doc["mode"] = telemetry.mode;
    doc["controller_state"] = telemetry.controllerState;
    doc["controller_reason"] = telemetry.controllerReason;
    doc["automatic_control_active"] = telemetry.automaticControlActive;
    doc["secondary_sensor_enabled"] = telemetry.secondarySensorEnabled;
    doc["control_sensor"] = telemetry.controlSensor;
    doc["beer_probe_present"] = telemetry.beerProbePresent;
    doc["beer_probe_valid"] = telemetry.beerProbeValid;
    doc["beer_probe_rom"] = telemetry.beerProbeRom;
    doc["chamber_probe_present"] = telemetry.chamberProbePresent;
    doc["chamber_probe_valid"] = telemetry.chamberProbeValid;
    doc["chamber_probe_rom"] = telemetry.chamberProbeRom;
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

    char payload[512];
    serializeJson(doc, payload, sizeof(payload));
    client_.publish(topicFor(config, "telemetry").c_str(), payload, false);
}

void MqttManager::publishKasaDiscovery(const SystemConfig& config, const String& devicePayload) {
    if (!client_.connected()) {
        return;
    }

    const String payload = "{\"device_id\":\"" + config.deviceId + "\",\"result\":" + devicePayload + "}";
    client_.publish(topicFor(config, "discovery/kasa").c_str(), payload.c_str(), false);
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
    const FermentationConfig& appliedConfig,
    const char* result,
    const char* message) {
    StaticJsonDocument<384> doc;
    doc["device_id"] = config.deviceId;
    doc["requested_version"] = appliedConfig.version;
    doc["applied_version"] = appliedConfig.version;
    doc["result"] = result;
    if (message != nullptr && message[0] != '\0') {
        doc["message"] = message;
    } else {
        doc["message"] = nullptr;
    }

    char payload[384];
    serializeJson(doc, payload, sizeof(payload));
    client_.publish(topicFor(config, "config/applied").c_str(), payload, false);
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
        Serial.printf("[mqtt] invalid system_config json error=%s\r\n", error.c_str());
        return;
    }

    SystemConfig updated = currentConfig;
    JsonObject heating = doc["heating"];
    if (!heating.isNull()) {
        updated.heatingOutput.driver =
            outputDriverFromString(String(static_cast<const char*>(heating["driver"] | "none")));
        updated.heatingOutput.host = String(static_cast<const char*>(heating["host"] | ""));
        updated.heatingOutput.port = heating["port"] | updated.heatingOutput.port;
        updated.heatingOutput.alias = String(static_cast<const char*>(heating["alias"] | ""));
    }

    JsonObject cooling = doc["cooling"];
    if (!cooling.isNull()) {
        updated.coolingOutput.driver =
            outputDriverFromString(String(static_cast<const char*>(cooling["driver"] | "none")));
        updated.coolingOutput.host = String(static_cast<const char*>(cooling["host"] | ""));
        updated.coolingOutput.port = cooling["port"] | updated.coolingOutput.port;
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
    systemConfigHandler_(updated);
}

void MqttManager::handleFermentationConfig(const String& payload) {
    if (!fermentationConfigHandler_) {
        return;
    }

    Serial.printf("[mqtt] fermentation config received bytes=%u\r\n", payload.length());

    StaticJsonDocument<1024> doc;
    const DeserializationError error = deserializeJson(doc, payload);
    if (error) {
        Serial.printf("[mqtt] invalid fermentation config json error=%s\r\n", error.c_str());
        return;
    }

    const uint32_t schemaVersion = doc["schema_version"] | 0;
    const uint32_t version = doc["version"] | 0;
    const String deviceId = String(static_cast<const char*>(doc["device_id"] | ""));
    const String mode = String(static_cast<const char*>(doc["mode"] | ""));
    JsonObject thermostat = doc["thermostat"];
    JsonObject sensors = doc["sensors"];

    if (schemaVersion != 1 || version == 0 || deviceId.isEmpty() || thermostat.isNull() || sensors.isNull()) {
        Serial.println("[mqtt] fermentation config missing required fields");
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
        Serial.println("[mqtt] fermentation config validation failed");
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
    fermentationConfigHandler_(updated);
}

void MqttManager::handleCommand(const String& payload) {
    StaticJsonDocument<384> doc;
    const DeserializationError error = deserializeJson(doc, payload);
    if (error) {
        Serial.printf("[mqtt] invalid command json error=%s\r\n", error.c_str());
        return;
    }

    const String command = String(static_cast<const char*>(doc["command"] | ""));
    if (command == "discover_kasa") {
        if (discoveryRequestHandler_) {
            discoveryRequestHandler_();
        }
        return;
    }

    if (command == "set_output" && outputCommandHandler_) {
        const String target = String(static_cast<const char*>(doc["target"] | ""));
        const String state = String(static_cast<const char*>(doc["state"] | ""));
        outputCommandHandler_(target, outputStateFromString(state));
        return;
    }

    if ((command == "check_update" || command == "start_update") && otaCommandHandler_) {
        JsonObject args = doc["args"];
        const String channel = args.isNull()
            ? String(static_cast<const char*>(doc["channel"] | ""))
            : String(static_cast<const char*>(args["channel"] | ""));
        otaCommandHandler_(command, channel);
    }
}

String MqttManager::topicFor(const SystemConfig& config, const char* suffix) const {
    return config.mqtt.topicPrefix + "/" + config.deviceId + "/" + suffix;
}
