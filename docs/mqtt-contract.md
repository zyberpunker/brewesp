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
  "temp_secondary_c": 17.1,
  "temp_beer_c": 18.42,
  "temp_chamber_c": 17.1,
  "setpoint_c": 18.5,
  "effective_target_c": 18.7,
  "mode": "profile",
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
  "ts": "2026-03-29T14:00:00Z",
  "mode": "profile",
  "setpoint_c": 18.5,
  "hysteresis_c": 0.3,
  "cooling_delay_s": 300,
  "heating_delay_s": 120,
  "fw_version": "0.1.0",
  "ota_status": "idle",
  "heating": false,
  "cooling": false,
  "active_config_version": 4,
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
  "message": "profile.steps[1].advance_policy must be auto or manual_release"
}
```

### `command`

Imperative actions that should not live inside retained config.

Profile runtime commands:

- `profile_pause`
- `profile_resume`
- `profile_release_hold`
- `profile_jump_to_step`
- `profile_stop`

Other commands may still exist for operations such as OTA or direct output
control, but profile runtime commands are the active contract for profile
execution control.

Examples:

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

Example event payload:

```json
{
  "device_id": "fermenter-01",
  "ts": "2026-03-29T14:06:00Z",
  "event": "ota_update_completed",
  "fw_version": "0.2.0",
  "result": "ok",
  "message": null
}
```

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
