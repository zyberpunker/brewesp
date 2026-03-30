# brewesp

ESP32-based fermentation controller inspired by STC-1000/STC-1000+.

The goal is to keep the simple thermostat/profile model from STC-1000+,
but modernize the system around an ESP32, MQTT, and a Dockerized web service.

## Project direction

The first planning pass in this repository is based on the STC-1000+ user
manual:

- Original manual: [STC-1000+ user manual](https://matsstaff.github.io/stc1000p/usermanual/)
- Project home: [stc1000p](https://matsstaff.github.io/stc1000p/)

The ESP32 platform we are targeting is an `ESP32-D0WD-V3` with:

- dual core CPU at 240 MHz
- Wi-Fi and Bluetooth
- 4 MB flash
- 40 MHz crystal

That is enough for a robust controller with:

- dual-stage thermostat control (heating + cooling)
- MQTT state/config integration
- persisted runtime state after power loss
- OTA update support
- a small companion web service for settings and history

## Planned architecture

The system is split into three parts:

1. `firmware/`
   ESP32 firmware that reads sensors, runs the control loop, applies profiles,
   and publishes telemetry/state over MQTT.
2. `services/web/`
   Dockerized web service that provides settings UI, device overview, profile
   editor, and history pages. It also publishes config JSON to MQTT.
3. `docs/`
   Design notes, requirements, MQTT contract, and roadmap.

## Current planning documents

- [docs/stc1000-analysis.md](C:\Users\ola\git\brewesp\docs\stc1000-analysis.md)
- [docs/architecture.md](C:\Users\ola\git\brewesp\docs\architecture.md)
- [docs/mqtt-contract.md](C:\Users\ola\git\brewesp\docs\mqtt-contract.md)
- [docs/hardware-ui.md](C:\Users\ola\git\brewesp\docs\hardware-ui.md)
- [docs/kasa-integration.md](C:\Users\ola\git\brewesp\docs\kasa-integration.md)
- [docs/windows-usb-and-ota.md](C:\Users\ola\git\brewesp\docs\windows-usb-and-ota.md)
- [docs/wiring.md](C:\Users\ola\git\brewesp\docs\wiring.md)
- [system-config.schema.json](C:\Users\ola\git\brewesp\docs\schemas\system-config.schema.json)
- [fermentation-config.schema.json](C:\Users\ola\git\brewesp\docs\schemas\fermentation-config.schema.json)

## Initial decisions

- Preserve the STC mental model: setpoint, hysteresis, heating delay, cooling
  delay, profiles, ramping, thermostat mode.
- Replace EEPROM-oriented remote control with JSON payloads over MQTT.
- Keep two probes in scope from the start: product probe and chamber probe.
- Use the beer/product probe as the primary control sensor and the chamber
  probe as the secondary diagnostic/limiting sensor.
- Let the web service be the primary settings UI.
- Use an `AZ-Touch MOD` touchscreen panel as the planned local operator UI.
- Use Docker for the companion services, with MQTT broker + web service +
  database.

## Suggested next implementation step

Build the first vertical slice:

- firmware reads one or two DS18B20 sensors
- firmware publishes telemetry to MQTT
- web service subscribes and stores history
- web service publishes retained config JSON
- firmware applies setpoint/hysteresis from MQTT config

OTA is now wired in the repo:

- web service exposes a firmware manifest and serves the mounted firmware `.bin`
- MQTT commands trigger update checks or update start
- ESP32 downloads and applies firmware over Wi-Fi using OTA partitions
- HTTPS OTA requires a configured certificate fingerprint; plain HTTP must be explicitly allowed

## Recovery AP defaults

If the device has no saved Wi-Fi configuration, it starts a recovery access
point for onboarding.

- SSID pattern: `brewesp-setup-<suffix>`
- default password: `brewesp123`
- default setup address: `192.168.4.1`
