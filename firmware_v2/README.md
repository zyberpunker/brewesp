# Firmware v2

Fresh ESP32 firmware rewrite for `0.2.0-dev`.

This tree is intentionally separate from the legacy firmware so the new control
path can be rebuilt from the repository documentation without inheriting old
implementation problems.

Current scope in this rewrite:

- local `system_config` persisted in NVS
- recovery AP with onboarding page for Wi-Fi and MQTT bootstrap
- cached `fermentation_config` persisted in NVS
- DS18B20 beer/chamber probe handling on a shared OneWire bus
- thermostat control with heating/cooling hysteresis, delays, and fault shutdown
- pluggable output drivers for `gpio`, `shelly_http_rpc`, and `kasa_local`
- MQTT availability, heartbeat, state, telemetry, and config apply handling

Current transport limits in `firmware_v2`:

- MQTT uses plain TCP only for now; `mqtt.tls=true` is rejected
- Shelly output uses HTTP RPC only for now; `https=true` is rejected

Initial build commands:

```powershell
cd C:\Users\ola\git\brewesp\firmware_v2
pio run
pio run -t upload --upload-port COM4
pio device monitor --port COM4 --baud 115200
```
