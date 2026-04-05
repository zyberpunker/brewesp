# Firmware v2

Fresh ESP32 firmware rewrite for `0.2.0-dev`.

This tree is intentionally separate from the legacy firmware so the new control
path can be rebuilt from the repository documentation without inheriting old
implementation problems.

Current scope in this rewrite:

- local `system_config` persisted in NVS
- explicit `config_owner` in `system_config` with `local` or `external` runtime ownership
- recovery AP with onboarding page for Wi-Fi and MQTT bootstrap
- minimal local HTTP control-owner page/API in normal operation for live owner switching without reboot
- cached `fermentation_config` persisted in NVS
- local `manual`, `thermostat`, and `profile` fermentation modes
- local profile runtime persisted in NVS for reboot recovery
- DS18B20 beer/chamber probe handling on a shared OneWire bus
- thermostat control with heating/cooling hysteresis, delays, and fault shutdown
- pluggable output drivers for `gpio`, `shelly_http_rpc`, and `kasa_local`
- MQTT availability, heartbeat, state, telemetry, config apply, and profile control handling

Current transport limits in `firmware_v2`:

- MQTT uses plain TCP only for now; `mqtt.tls=true` is rejected
- Shelly output uses HTTP RPC only for now; `https=true` is rejected

Current ownership behavior:

- `config_owner=external` keeps MQTT config and command handling enabled
- `config_owner=local` disables inbound MQTT control and allows local-only drift ownership
- switching owner over the local HTTP interface does not reboot the ESP32
- switching to `external` fails in place if Wi-Fi or MQTT connectivity is not ready

Current local owner endpoints in normal operation:

- `GET /` simple owner toggle page
- `GET /api/runtime/state` current owner/runtime summary
- `POST /api/control-owner` JSON body `{"owner":"local"}` or `{"owner":"external"}`

Initial build commands:

```powershell
cd C:\Users\ola\git\brewesp\firmware_v2
pio run
pio run -t upload --upload-port COM4
pio device monitor --port COM4 --baud 115200
```
