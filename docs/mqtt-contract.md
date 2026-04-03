# MQTT contract v2

This document defines the active MQTT topic and payload contract for BrewESP.

The main goal is:

- telemetry and history from ESP32 to the backend
- desired fermentation configuration from backend to ESP32
- explicit acknowledgement of what the device actually applied
- runtime truth for profile progress owned by the ESP32

Important scope note:

- MQTT carries fermentation/process config and runtime coordination
- bootstrap/system config must not rely exclusively on MQTT
- physical actuation remains local to the ESP32 and its selected output backend
- invalid desired config must be rejected without changing relay behavior

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
  "uptime_s": 86400,
  "wifi_rssi": -58,
  "heap_free": 183424,
  "ui": "headless",
  "heating": "off",
  "cooling": "off"
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
  "ts": 1743343200,
  "temp_primary_c": 18.42,
  "temp_secondary_c": 17.1,
  "setpoint_c": 18.5,
  "effective_target_c": 18.7,
  "mode": "profile",
  "controller_state": "idle",
  "controller_reason": "within hysteresis",
  "automatic_control_active": true,
  "active_config_version": 4,
  "secondary_sensor_enabled": false,
  "control_sensor": "primary",
  "beer_probe_present": true,
  "beer_probe_valid": true,
  "beer_probe_stale": false,
  "beer_probe_rom": "28ff112233445566",
  "chamber_probe_present": true,
  "chamber_probe_valid": true,
  "chamber_probe_stale": false,
  "chamber_probe_rom": "28ffaa9988776655",
  "profile_id": "ale-primary",
  "profile_step_id": "rise",
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
- profile runtime state changed
- every 5 minutes as a state refresh

Payload example:

```json
{
  "device_id": "fermenter-01",
  "ui": "headless",
  "mode": "profile",
  "setpoint_c": 18.5,
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
  "cooling": "off",
  "heating_desc": "gpio 25 off",
  "cooling_desc": "kasa 192.168.1.88 off",
  "controller_state": "idle",
  "controller_reason": "within hysteresis",
  "automatic_control_active": true,
  "active_config_version": 4,
  "secondary_sensor_enabled": false,
  "control_sensor": "primary",
  "beer_probe_present": true,
  "beer_probe_valid": true,
  "beer_probe_stale": false,
  "beer_probe_rom": "28ff112233445566",
  "chamber_probe_present": true,
  "chamber_probe_valid": true,
  "chamber_probe_stale": false,
  "chamber_probe_rom": "28ffaa9988776655",
  "profile_runtime": {
    "active_profile_id": "ale-primary",
    "active_step_id": "rise",
    "active_step_index": 1,
    "phase": "ramping",
    "step_started_at": 86400,
    "step_hold_started_at": null,
    "effective_target_c": 18.7,
    "waiting_for_manual_release": false,
    "paused": false
  },
  "fault": null
}
```

Additional OTA fields:

- `ota_target_version` is included when a manifest check found a newer image
- `ota_message` is included when firmware has a useful human-readable OTA result
- `ota_progress_pct` is currently coarse-grained; the current firmware reports `100`
  when the new image is staged and reboot is pending, otherwise `0`
- `heating` and `cooling` in `state` are string enums: `on`, `off`, or `unknown`
- `fault` in `state` and `telemetry` is either `null` or a short string such as
  `beer sensor stale` or `chamber sensor missing`
- `beer_probe_stale` and `chamber_probe_stale` indicate firmware-side stale detection;
  the current firmware uses a fixed `30` second timeout for this fault model

`profile_runtime` is the runtime truth for an active profile. Frontends should
use it instead of inferring progress from desired config alone.
`step_started_at` and `step_hold_started_at` are controller uptime seconds, not
wall-clock timestamps. `step_hold_started_at` may be `null` until the hold
phase begins.

Expected `profile_runtime.phase` values:

- `idle`
- `ramping`
- `holding`
- `waiting_manual_release`
- `paused`
- `completed`
- `faulted`

### `history/raw`

Optional raw event stream for archival or analytics.

This can initially be identical to `telemetry` and later be split if needed.

### Sensor fault semantics

- the controller publishes per-probe `present`, `valid`, and `stale` flags for
  both beer and chamber probes
- `fault` is controller-scoped, not a generic alarm bucket
- `fault` becomes non-null only when the selected primary control sensor is
  missing, invalid, or stale
- when `fault` is non-null, the firmware forces both outputs off locally
- a non-primary probe may still report `stale` or `invalid` without forcing a
  controller fault by itself

### `config/desired`

Retained JSON document published by the web service.

This topic is for `fermentation_config`, not full device bootstrap settings.

The active config schema is
[fermentation-config.schema.json](C:/Users/ola/git/brewesp/docs/schemas/fermentation-config.schema.json).

Payload example:

```json
{
  "schema_version": 2,
  "version": 4,
  "device_id": "fermenter-01",
  "name": "Ale primary",
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
    "secondary_limit_hysteresis_c": 1.5,
    "control_sensor": "primary"
  },
  "alarms": {
    "deviation_c": 2.0,
    "sensor_stale_s": 30
  },
  "profile": {
    "id": "ale-primary",
    "steps": [
      {
        "id": "pitch",
        "label": "Pitch",
        "target_c": 18.0,
        "hold_duration_s": 259200,
        "advance_policy": "auto"
      },
      {
        "id": "rise",
        "label": "Rise to diacetyl rest",
        "target_c": 20.0,
        "ramp_duration_s": 43200,
        "hold_duration_s": 172800,
        "advance_policy": "auto"
      },
      {
        "id": "cold-crash-ready",
        "label": "Wait for operator release",
        "target_c": 20.0,
        "hold_duration_s": 0,
        "advance_policy": "manual_release"
      },
      {
        "id": "cold-crash",
        "label": "Cold crash",
        "target_c": 4.0,
        "ramp_duration_s": 86400,
        "hold_duration_s": 86400,
        "advance_policy": "auto"
      }
    ]
  }
}
```

Wire-unit rule:

- `target_c` is always Celsius
- `hold_duration_s` and `ramp_duration_s` are always integer seconds
- editors may use friendlier display units, but serialization must stay
  unambiguous on the wire

### `config/applied`

Published by the ESP32 after validation and activation of a config version.

Payload example:

```json
{
  "device_id": "fermenter-01",
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
  "requested_version": 4,
  "applied_version": 3,
  "result": "error",
  "message": "profile.steps[1].advance_policy must be auto or manual_release"
}
```

### `command`

Imperative actions that should not live inside retained config.

Supported commands:

- `set_output`
- `discover_outputs`
- `profile_pause`
- `profile_resume`
- `profile_release_hold`
- `profile_jump_to_step`
- `profile_stop`
- `check_update`
- `start_update`

Profile runtime commands are the active contract for profile execution control.
For OTA commands, the requested channel may be provided either as
`args.channel` or as a top-level `channel`. Firmware prefers `args.channel`
when both are present.

`set_output` is an operator override for direct heating/cooling testing. It
temporarily suspends automatic control on the ESP32 for about 2 minutes so the
requested output state can remain visible instead of being immediately
overridden by thermostat or profile control. Sensor-fault shutdown and OTA
lockout still force both outputs off.

`discover_outputs` is intended for operator-initiated LAN scans of supported
output backends such as Kasa and Shelly. Discovery results are published on the
non-retained topic `<topic_prefix>/<device_id>/discovery/output`.

Examples:

```json
{
  "command": "set_output",
  "requested_by": "web",
  "ts": "2026-04-03T13:10:00Z",
  "target": "heating",
  "state": "on"
}
```

```json
{
  "command": "discover_outputs",
  "requested_by": "web",
  "ts": "2026-04-03T12:40:00Z"
}
```

```json
{
  "command": "profile_pause",
  "requested_by": "web",
  "ts": "2026-03-29T14:05:00Z"
}
```

```json
{
  "command": "profile_resume",
  "requested_by": "web",
  "ts": "2026-03-29T14:06:00Z"
}
```

```json
{
  "command": "profile_release_hold",
  "requested_by": "web",
  "ts": "2026-03-29T14:07:00Z"
}
```

```json
{
  "command": "profile_jump_to_step",
  "requested_by": "web",
  "ts": "2026-03-29T14:08:00Z",
  "args": {
    "step_id": "cold-crash"
  }
}
```

```json
{
  "command": "profile_stop",
  "requested_by": "web",
  "ts": "2026-03-29T14:09:00Z"
}
```

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
10. `profile_runtime` is device-owned runtime truth.
11. OTA binaries should be transferred over HTTP/HTTPS, not MQTT payloads.
