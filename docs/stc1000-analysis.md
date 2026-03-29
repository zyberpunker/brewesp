# STC-1000+ analysis for an ESP32 version

This document captures the STC-1000+ functions that are relevant for an ESP32
rewrite and how they should map into a modern architecture.

Source used:

- STC-1000+ user manual: https://matsstaff.github.io/stc1000p/usermanual/

## Core STC-1000+ functions

The original firmware provides these main features:

- thermostat mode with a setpoint (`SP`)
- dual-stage outputs for heating and cooling
- hysteresis around the setpoint (`hy`)
- independent minimum off-delay for cooling (`cd`) and heating (`hd`)
- up to 6 profiles with up to 10 setpoints each
- per-step durations up to 999 hours
- optional ramping between profile steps
- calibration offset for the primary probe (`tc`)
- optional secondary probe with its own offset (`tc2`) and hysteresis (`hy2`)
- run mode switching between thermostat mode and profile mode
- alarm when temperature is outside or inside a defined band (`SA`)
- progress persistence after power loss
- simple remote read/write support in the communication build

## Functions to preserve in the ESP32 version

These should remain first-class requirements:

1. Dual-stage temperature control
   One relay output for cooling and one for heating, with hard interlocking so
   they can never be active at the same time.
2. Thermostat mode
   Maintain a fixed setpoint with hysteresis and per-output compressor/heater
   delay logic.
3. Profile mode
   Run a sequence of setpoints over time and fall back to thermostat mode after
   the profile ends.
4. Ramping
   Preserve the user-facing concept, but implement it more accurately than the
   original hourly approximation.
5. Two-probe support
   Primary probe for product/beer temperature, secondary probe for chamber
   temperature limiting and diagnostics.
6. Calibration
   Independent offset calibration per sensor.
7. Resume after power loss
   Persist active mode, active profile step, elapsed time, and active setpoint.
8. Alarming
   Detect sensor faults, temperature deviation, and control safety issues.

## Functions to replace or modernize

The original hardware and firmware had several constraints that do not need to
be copied directly.

### Replace button-driven settings with web + MQTT

The STC-1000+ menu system was optimized for four buttons and a segmented
display. For the ESP32 version:

- web UI becomes the main configuration surface
- MQTT becomes the control/config transport
- local buttons/display become optional future hardware, not a core dependency

### Replace EEPROM address protocol with JSON config

The communication firmware exposed low-level EEPROM reads/writes. That should
be replaced with a versioned JSON contract:

- easier to validate
- easier to audit
- backward-compatible evolution is simpler
- easier for a web service to publish safely

### Improve ramping resolution

The original firmware updated profile state on hourly boundaries. On ESP32 we
should use a finer timebase, for example:

- internal scheduler tick every second
- profile/ramp evaluation every minute
- persisted timestamps using wall clock or monotonic runtime

That will make long ramps smoother and resume behavior more precise.

## Recommended new ESP32-specific features

These are not part of STC-1000+, but fit the platform well:

- pluggable output backends, not only GPIO relays
- OTA firmware update
- NTP time synchronization
- Wi-Fi station mode with optional fallback AP for recovery
- MQTT last will / availability topic
- richer telemetry and history publishing
- sensor fault detection and fail-safe relay shutdown
- config versioning and apply acknowledgements
- optional local web diagnostics page on the ESP32 itself

## Proposed feature priorities

### Phase 1: minimum viable controller

- one device
- one or two temperature probes
- thermostat mode
- MQTT telemetry
- MQTT config apply
- persistent config in NVS
- cooling/heating delays
- fail-safe relay logic

### Phase 2: fermentation controller parity

- profile mode
- ramping
- pause/resume
- power-loss recovery
- alarms
- web history and profile editor

### Phase 3: operational polish

- OTA
- local recovery AP
- auth/roles in web service
- export/import profiles
- notifications

## Important behavior decisions

These should be treated as explicit product decisions, not left implicit.

### Control variable

The primary control variable should be the product probe temperature.

Reason:

- that matches fermentation use best
- chamber probe works better as a limiting signal than as the main target

### Sensor safety

If the primary sensor is missing or invalid:

- both relays must turn off
- the device must publish a fault state
- the web UI must show the controller as degraded

### Relay safety

Never allow heating and cooling to be on simultaneously, even if config or code
errors occur.

### Output abstraction

The original STC-1000 drives physical outputs directly. The ESP32 version
should not hard-code that assumption.

Instead, define logical outputs:

- `heating`
- `cooling`

and map them to interchangeable output drivers, such as:

- local GPIO relay
- Shelly Plug S Gen3 over local RPC/HTTP
- TP-Link Kasa KP105 over local LAN protocol
- generic MQTT-controlled relay in the future
- other Wi-Fi relay integrations later

This gives us one control engine and multiple transport implementations.

### Config updates while running

Config changes should be versioned and applied atomically. Profile edits during
an active run should have clear semantics:

- current step target may remain until next schedule boundary
- future steps should use the new data
- the device should publish which version is active
