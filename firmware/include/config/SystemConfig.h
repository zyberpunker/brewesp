#pragma once

#include <Arduino.h>

enum class OutputDriverType {
    None,
    Gpio,
    ShellyHttpRpc,
    KasaLocal,
    GenericMqttRelay,
};

struct LocalUiConfig {
    bool enabled = false;
    bool headlessAllowed = true;
};

struct DisplayConfig {
    bool enabled = false;
    String driver = "ssd1306";
    String i2cAddress = "0x3C";
    uint32_t dimAfterSeconds = 30;
    uint32_t offAfterSeconds = 120;
    bool wakeOnAlarm = true;
    bool wakeRequiresSecondPress = true;
};

struct ButtonsConfig {
    bool enabled = false;
    uint8_t upGpio = 32;
    uint8_t downGpio = 33;
    uint8_t selectGpio = 25;
    uint8_t backGpio = 26;
    bool activeLow = true;
};

struct OutputConfig {
    OutputDriverType driver = OutputDriverType::None;
    int8_t pin = -1;
    bool activeHigh = true;
    String host;
    uint16_t port = 80;
    uint8_t switchId = 0;
    String username;
    String password;
    String alias;
    uint32_t pollIntervalSeconds = 30;
};

struct RecoveryApConfig {
    bool enabled = true;
    String ssid;
    String password = "brewesp123";
    String ip = "192.168.4.1";
    bool autoStartWhenUnprovisioned = true;
    uint32_t startAfterWifiFailureSeconds = 180;
};

struct WifiConfig {
    String ssid;
    String password;
    RecoveryApConfig recoveryAp;
};

struct MqttConfig {
    String host;
    uint16_t port = 1883;
    String clientId = "brewesp-dev";
    String username;
    String password;
    String topicPrefix = "brewesp";
};

struct HeartbeatConfig {
    uint32_t intervalSeconds = 60;
};

struct SensorBusConfig {
    uint8_t oneWireGpio = 32;
    uint32_t pollIntervalSeconds = 5;
    String beerProbeRom;
    String chamberProbeRom;
};

struct SystemConfig {
    String deviceId = "brewesp-dev";
    WifiConfig wifi;
    MqttConfig mqtt;
    HeartbeatConfig heartbeat;
    SensorBusConfig sensors;
    LocalUiConfig localUi;
    DisplayConfig display;
    ButtonsConfig buttons;
    OutputConfig heatingOutput;
    OutputConfig coolingOutput;
};
