# Firmware

Planned home for the ESP32 controller firmware.

Intended stack:

- PlatformIO
- Arduino framework for ESP32
- DS18B20 temperature probes
- MQTT over Wi-Fi
- NVS for persisted config/state

The first implementation target is:

- read sensors
- apply thermostat logic
- publish telemetry/state to MQTT
- consume retained config JSON from MQTT

Current scaffold now includes:

- PlatformIO Arduino project setup
- `OutputDriver` abstraction
- `gpio` output driver
- `shelly_http_rpc` output driver stub
- `kasa_local` output driver stub
- optional `local_ui` manager for headless or panel-based operation
