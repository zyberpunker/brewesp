#include "network/MqttManager.h"

#include <math.h>

#include <ArduinoJson.h>
#include <WiFi.h>

#include "output/OutputManager.h"
#include "ui/LocalUiManager.h"

namespace {
const char* kFirmwareVersion = "0.1.0-dev";
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
        + "\",\"fw_version\":\"" + kFirmwareVersion + "\"}";
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

    StaticJsonDocument<640> doc;
    doc["device_id"] = config.deviceId;
    doc["fw_version"] = kFirmwareVersion;
    doc["ui"] = localUi.isHeadless() ? "headless" : "local";
    doc["mode"] = telemetry.mode;
    doc["heating"] = outputStateName(outputs.heatingState());
    doc["cooling"] = outputStateName(outputs.coolingState());
    doc["heating_desc"] = heatingDescription;
    doc["cooling_desc"] = coolingDescription;
    doc["controller_state"] = telemetry.controllerState;
    doc["controller_reason"] = telemetry.controllerReason;
    doc["automatic_control_active"] = telemetry.automaticControlActive;
    if (telemetry.hasSetpoint) {
        doc["setpoint_c"] = telemetry.setpointC;
    } else {
        doc["setpoint_c"] = nullptr;
    }
    doc["hysteresis_c"] = telemetry.hysteresisC;
    doc["cooling_delay_s"] = telemetry.coolingDelaySeconds;
    doc["heating_delay_s"] = telemetry.heatingDelaySeconds;

    char payload[640];
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

    if (schemaVersion != 1 || version == 0 || deviceId.isEmpty() || thermostat.isNull()) {
        Serial.println("[mqtt] fermentation config missing required fields");
        return;
    }

    const float setpointC = thermostat["setpoint_c"] | NAN;
    const float hysteresisC = thermostat["hysteresis_c"] | NAN;
    const uint16_t coolingDelaySeconds = thermostat["cooling_delay_s"] | 0;
    const uint16_t heatingDelaySeconds = thermostat["heating_delay_s"] | 0;

    if (
        isnan(setpointC) || setpointC < -20.0f || setpointC > 50.0f || isnan(hysteresisC)
        || hysteresisC < 0.1f || hysteresisC > 5.0f || coolingDelaySeconds > 3600
        || heatingDelaySeconds > 3600 || (mode != "thermostat" && mode != "profile")) {
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
    }
}

String MqttManager::topicFor(const SystemConfig& config, const char* suffix) const {
    return config.mqtt.topicPrefix + "/" + config.deviceId + "/" + suffix;
}
