#pragma once

#include <Arduino.h>

struct ThermostatConfig {
    float setpointC = 20.0f;
    float hysteresisC = 0.3f;
    uint16_t coolingDelaySeconds = 300;
    uint16_t heatingDelaySeconds = 120;
};

struct FermentationConfig {
    uint32_t schemaVersion = 1;
    uint32_t version = 1;
    String deviceId = "brewesp-dev";
    String name = "Default fermentation";
    String mode = "thermostat";
    ThermostatConfig thermostat;
};
