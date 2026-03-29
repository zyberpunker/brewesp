#pragma once

#include <Arduino.h>

struct ThermostatConfig {
    float setpointC = 20.0f;
    float hysteresisC = 0.3f;
    uint16_t coolingDelaySeconds = 300;
    uint16_t heatingDelaySeconds = 120;
};

struct SensorControlConfig {
    float primaryOffsetC = 0.0f;
    bool secondaryEnabled = false;
    float secondaryOffsetC = 0.0f;
    float secondaryLimitHysteresisC = 1.5f;
    String controlSensor = "primary";
};

struct FermentationConfig {
    uint32_t schemaVersion = 1;
    uint32_t version = 1;
    String deviceId = "brewesp-dev";
    String name = "Default fermentation";
    String mode = "thermostat";
    ThermostatConfig thermostat;
    SensorControlConfig sensors;
};
