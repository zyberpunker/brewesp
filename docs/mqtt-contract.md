# MQTT contract v0

This is the first proposed topic and payload design for the controller.

The main goal is:

- telemetry and history from ESP32 to the backend
- desired fermentation configuration from backend to ESP32
- explicit acknowledgement of what the device actually applied

Important scope note:

- MQTT should carry fermentation/process config
- bootstrap/system config should not rely exclusively on MQTT
- physical actuation should remain local to the ESP32 and its selected output
  backend
- secondary chamber probe support should be optional and enable-able in config

## Topic layout

Assume `device_id = fermenter-01`.

- `brewesp/fermenter-01/availability`
- `brewesp/fermenter-01/heartbeat`
- `brewesp/fermenter-01/state`
- `brewesp/fermenter-01/telemetry`
- `brewesp/fermenter-01/history/raw`
- `brewesp/fermenter-01/config/desired`
- `brewesp/fermenter-01/config/applied`
- `brewesp/fermenter-01/command`
- `brewesp/fermenter-01/event`

## Topic intent

### `availability`

Online/offline status using MQTT last will.

Payload example:

```json
{
  "device_id": "fermenter-01",
  "status": "online",
  "fw_version": "0.1.0"
}
```

### `heartbeat`

Lightweight liveness signal for monitoring.

Recommended publish interval:

- every 60 seconds for normal mains-powered operation

Why not 5 minutes:

- 5 minutes is workable for low-noise monitoring
- but it is slow if you want to notice lockups, Wi-Fi loss, or broker issues
- this device is mains-powered, so a 60-second heartbeat is cheap

Recommended stale/offline rules in the backend:

- mark as `stale` after 2 missed heartbeats
- mark as `offline` after 3 missed heartbeats if no MQTT last will was seen
- treat MQTT last will `offline` as immediate offline

Payload example:

```json
{
  "device_id": "fermenter-01",
  "ts": "2026-03-29T14:00:00Z",
  "uptime_s": 86400,
  "wifi_rssi": -58,
  "heap_free": 183424,
  "active_config_version": 4,
  "fault": null
}
```

### `telemetry`

Frequent runtime measurements for dashboards.

Suggested publish interval:

- every 10 to 30 seconds during normal operation

Payload example:

```json
{
  "device_id": "fermenter-01",
  "ts": "2026-03-29T14:00:00Z",
  "temp_primary_c": 18.42,
  "temp_secondary_c": 17.10,
  "temp_beer_c": 18.42,
  "temp_chamber_c": 17.10,
  "setpoint_c": 18.50,
  "mode": "profile",
  "profile_id": "ale-primary",
  "profile_step": 2,
  "heating": false,
  "cooling": false,
  "fault": null
}
```

### `state`

Lower-frequency but richer controller state.

Suggested publish triggers:

- boot
- config applied
- mode changed
- fault changed
- every 5 minutes as a state refresh

Payload example:

```json
{
  "device_id": "fermenter-01",
  "ui": "headless",
  "mode": "thermostat",
  "setpoint_c": 19.0,
  "hysteresis_c": 0.3,
  "cooling_delay_s": 300,
  "heating_delay_s": 120,
  "fw_version": "0.1.0",
  "ota_status": "idle",
  "ota_channel": "stable",
  "ota_available": false,
  "ota_progress_pct": 0,
  "ota_reboot_pending": false,
  "heating": "off",
  "cooling": "on",
  "heating_desc": "gpio 25 off",
  "cooling_desc": "kasa 192.168.1.88 on",
  "controller_state": "cooling",
  "controller_reason": "primary_above_setpoint",
  "automatic_control_active": true,
  "secondary_sensor_enabled": false,
  "control_sensor": "primary",
  "beer_probe_present": true,
  "beer_probe_valid": true,
  "beer_probe_rom": "28ff112233445566",
  "chamber_probe_present": true,
  "chamber_probe_valid": true,
  "chamber_probe_rom": "28ffaa9988776655"
}
```

Additional OTA fields:

- `ota_target_version` is included when a manifest check found a newer image
- `ota_message` is included when firmware has a useful human-readable OTA result
- `ota_progress_pct` is currently coarse-grained; the current firmware reports `100`
  when the new image is staged and reboot is pending, otherwise `0`
- `heating` and `cooling` in `state` are string enums: `on`, `off`, or `unknown`

### `history/raw`

Optional raw event stream for archival or analytics.

This can initially be identical to `telemetry` and later be split if needed.

### `config/desired`

Retained JSON document published by the web service.

This topic is for `fermentation_config`, not full device bootstrap settings.

Payload example:

```json
{
  "version": 4,
  "device_id": "fermenter-01",
  "mode": "profile",
  "thermostat": {
    "setpoint_c": 18.5,
    "hysteresis_c": 0.3,
    "cooling_delay_s": 300,
    "heating_delay_s": 120
  },
  "sensors": {
    "primary_offset_c": 0.0,
    "secondary_enabled": true,
    "secondary_offset_c": -0.2,
    "secondary_limit_hysteresis_c": 1.5
  },
  "alarms": {
    "deviation_c": 2.0
  },
  "profile": {
    "id": "ale-primary",
    "ramping": true,
    "steps": [
      { "target_c": 18.0, "duration_h": 72 },
      { "target_c": 20.0, "duration_h": 48 },
      { "target_c": 4.0, "duration_h": 24 }
    ]
  }
}
```

### `config/applied`

Published by the ESP32 after validation and activation of a config version.

Payload example:

```json
{
  "device_id": "fermenter-01",
  "ts": "2026-03-29T14:00:03Z",
  "requested_version": 4,
  "applied_version": 4,
  "result": "ok",
  "message": null
}
```

If validation fails:

```json
{
  "device_id": "fermenter-01",
  "ts": "2026-03-29T14:00:03Z",
  "requested_version": 4,
  "applied_version": 3,
  "result": "error",
  "message": "cooling_delay_s must be between 0 and 3600"
}
```

### `command`

Imperative actions that should not live inside retained config.

Examples:

- pause active profile
- resume active profile
- stop profile and switch to thermostat
- check for firmware update
- start firmware update
- reboot device

Payload example:

```json
{
  "command": "start_update",
  "requested_by": "web",
  "ts": "2026-03-29T14:05:00Z",
  "args": {
    "channel": "stable"
  }
}
```

Current OTA-specific commands accepted by firmware:

- `check_update`
- `start_update`

For OTA commands, the requested channel may be provided either as
`args.channel` or as a top-level `channel`. Firmware prefers `args.channel`
when both are present.

## System config recommendation

Do not make MQTT the only path for:

- Wi-Fi provisioning
- MQTT broker credentials
- output backend selection
- Shelly IP/host and auth
- GPIO pin mapping
- OneWire sensor bus GPIO and optional sensor ROM mapping

Reason:

- if Wi-Fi or MQTT settings are wrong, the device cannot recover through MQTT
- this is bootstrap/install configuration, not process control

Recommended approach:

- local onboarding API on the ESP32, exposed on LAN
- fallback recovery AP/captive portal if the device has no working network
- persist `system_config` in NVS
- optionally let the web service call the local API when the device is online

Headless operation:

- local panel/touchscreen must be optional
- the controller must remain operable over network interfaces without local UI

Schema files:

- `system_config`: [system-config.schema.json](C:\Users\ola\git\brewesp\docs\schemas\system-config.schema.json)
- `fermentation_config`: [fermentation-config.schema.json](C:\Users\ola\git\brewesp\docs\schemas\fermentation-config.schema.json)

Recommended provisioning flow:

1. Device boots without valid `system_config`.
2. Device starts a recovery AP.
3. User connects to the device locally and submits setup values.
4. Device saves `system_config` in NVS.
5. Device reboots and joins the configured Wi-Fi.

## OTA recommendation

Use MQTT only for OTA orchestration, not for the firmware binary itself.

Recommended model:

- MQTT command asks the ESP32 to check or start an update
- ESP32 fetches a firmware manifest over HTTP/HTTPS
- ESP32 downloads the `.bin` over HTTP/HTTPS
- ESP32 reports progress and final status on `state` and `event`
- scheduled checks may also run locally from saved OTA config
- HTTPS OTA requires `ca_cert_fingerprint`
- plain HTTP OTA requires `allow_http = true`

Example event payload:

```json
{
  "device_id": "fermenter-01",
  "ts": 1743343560,
  "event": "ota_update_completed",
  "fw_version": "0.2.0",
  "ota_status": "rebooting",
  "target_version": "0.2.0",
  "result": "ok",
  "message": null
}
```

OTA-related events currently emitted by firmware:

- `ota_check_completed`
- `ota_update_completed`

For `ota_check_completed`, `result` is one of:

- `update_available`
- `no_update`
- `error`

For `ota_update_completed`, `result` is one of:

- `ok`
- `no_update`
- `error`

## Contract rules

These rules are important from the start:

1. `config/desired` should be retained.
2. `config/applied` should not be retained.
3. Every desired config must carry a monotonic `version`.
4. The ESP32 must validate before applying.
5. Invalid config must never change relay behavior.
6. Commands are best-effort; physical safety always stays local to the device.
7. `availability` should use MQTT last will for immediate disconnect detection.
8. `heartbeat` should be lightweight and independent of telemetry/state payload size.
9. MQTT config topics are for `fermentation_config`, not mandatory bootstrap.
10. OTA binaries should be transferred over HTTP/HTTPS, not MQTT payloads.
