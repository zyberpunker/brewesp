# Hardware and local UI decisions

This document captures the current hardware direction for the first build.

## Chosen controller hardware

Use the existing ESP32 board already available in the project context:

- chip: `ESP32-D0WD-V3`
- dual core
- 240 MHz
- 4 MB flash

Reason:

- enough performance for control loop, MQTT, OTA, local touchscreen UI, and
  history telemetry
- no need to switch MCU platform now
- lower project risk than changing to a different ESP family

## Chosen local panel direction

Use an `AZ-Touch MOD` style wall panel with:

- `2.4"` TFT display
- `ILI9341` display controller
- `XPT2046` resistive touch controller
- built-in wall enclosure
- built-in buzzer
- direct fit for `ESP32 DevKitC` style boards

Sources used for the hardware assumption:

- AZ-Delivery product page: [AZ-Touch MOD 2.4"](https://www.az-delivery.de/en/products/az-touch-wandgehauseset-mit-touchscreen-fur-esp8266-und-esp32)
- openHASP hardware page: [AZ-Touch MOD](https://www.openhasp.com/0.7.0/hardware/az-delivery/az-touch/)
- AZ-Delivery wiring article: [AZ-Touch MOD with ESP32](https://www.az-delivery.uk/es/blogs/azdelivery-blog-fur-arduino-und-raspberry-pi/terminanzeige-mit-az-touch-mod-am-esp32-mqtt-node-red-und-esphome-teil-3)

## Chosen sensor direction

Use two `DS18B20` probes from the start:

1. `beer probe`
   attached to the fermenter or thermowell and used as the primary control
   variable
2. `chamber probe`
   placed in the chamber/fridge air and used for diagnostics, limiting, and
   later safety logic

This mirrors the STC-1000+ two-probe concept well, but with clearer role
separation for fermentation:

- beer probe controls the process
- chamber probe helps explain chamber behavior and allows smarter limiting

## Local UI direction

The touchscreen becomes the primary local operator interface.

Use:

- touch targets instead of four fixed hardware buttons
- one or more large on-screen controls for setpoint up/down
- simple page navigation optimized for operation while standing in front of the
  fermentation chamber

The web service remains the primary place for:

- full configuration
- profiles
- history
- device administration

The local panel should be optimized for:

- viewing current beer and chamber temperatures
- quick setpoint changes
- manual output override
- fault acknowledgement
- diagnostics and recovery guidance

## Headless mode requirements

The controller must still support running without the local panel connected.

That means:

- controller starts and runs normally without the touchscreen connected
- controller starts and runs normally without touch input available
- local UI features are enabled only when configured in `system_config`
- all critical control and configuration paths remain available through network
  interfaces

## Recommended UI model

The local touchscreen should be a small appliance UI, not a full duplicate of
the web app.

### Main screen

Show:

- beer temperature
- chamber temperature
- active setpoint
- active mode
- heating/cooling state
- controller state
- Wi-Fi and MQTT status
- alarm/fault indicator

### Quick actions

Expose large touch targets for:

- `Setpoint -`
- `Setpoint +`
- `Heat Off`
- `Cool Off`
- `Resume/Pause`
- `Diagnostics`

### Diagnostics

Show:

- firmware version
- uptime
- IP address
- RSSI
- output backend summary
- sensor status
- recovery/setup hints

## Locked v1 local UI behavior

This section defines the first implementation target for the touchscreen.

### Screens in v1

Use only three screens in the first version:

1. `home`
2. `manual`
3. `diagnostics`

The interaction model should stay shallow. Avoid nested touch menus in v1.

### Home screen contents

The home screen should show:

- beer temperature as the most prominent number
- chamber temperature as a secondary number
- active setpoint
- active mode: `thermostat` or `profile`
- output state: `heating`, `cooling`, or `idle`
- Wi-Fi status
- MQTT status
- fault or alarm marker

### Manual screen contents

The manual screen should show:

- `Setpoint -`
- `Setpoint +`
- `Disable Heating`
- `Disable Cooling`
- `Back`

### Diagnostics screen contents

The diagnostics screen should show:

- firmware version
- uptime
- IP address
- Wi-Fi RSSI
- MQTT connected/disconnected
- controller state
- output backend type for heating and cooling
- beer probe presence/status
- chamber probe presence/status

## Display sleep behavior in v1

The TFT backlight should not stay fully lit forever.

Default behavior:

- dim after `30` seconds of inactivity
- turn display off after `120` seconds of inactivity
- any touch wakes the display

To avoid accidental actions:

- if the display is off, the first touch wakes it
- that wake touch should not change setpoint or trigger a control action
- the next touch performs the intended action

Automatic wake events:

- new alarm or fault
- Wi-Fi disconnect
- MQTT disconnect

Display should stay awake while:

- the manual screen is open
- the diagnostics screen is open
- the user is actively changing setpoint
- an unacknowledged fault is on screen

## v1 local setpoint semantics

Local setpoint changes on the touchscreen should:

- update the active setpoint immediately on the device
- be treated as a local override of the running control target
- be published in device state so the web service can display it

The web service can later decide whether to persist that override into the main
fermentation configuration.

## v1 output disable semantics

`Disable Heating` and `Disable Cooling` are local operator overrides.

Behavior:

- disabling heating blocks automatic heating requests
- disabling cooling blocks automatic cooling requests
- disabling one output must not affect the other
- these overrides must be visible in MQTT state and on the home screen
- these overrides remain subject to sensor-fault shutdown and other safety
  rules

## Deferred to later versions

Do not include these in the first local touchscreen implementation:

- local profile editing
- local Wi-Fi provisioning
- local MQTT broker editing
- OTA initiation from the local screen
- historical graphs on-device
- keyboard-like text input on the panel

## Safety behavior for local controls

Local controls must not bypass controller safety rules.

That means:

- never force heating and cooling on together
- still respect minimum heating/cooling delay timers
- still shut outputs down on sensor fault
- local disable of heating/cooling should override automatic requests

## MVP scope for local UI

Phase 1 local UI should support:

- show beer and chamber temperatures
- raise/lower active setpoint
- show Wi-Fi/MQTT connectivity
- show faults
- support simple touch navigation between `home`, `manual`, and `diagnostics`

Phase 2 can add:

- profile pause/resume
- chamber limit settings
- local network diagnostics
- local onboarding helpers

## Provisioning and recovery interaction

The local panel should support recovery, but it is not the primary provisioning
surface.

Recommended behavior:

- first-time setup happens through the ESP32 recovery AP and web page
- the touchscreen should show that setup mode is active
- the touchscreen should display the recovery SSID and IP address
- a small hidden service button is still recommended for forced recovery during
  boot, even if the normal operator interaction is touch-based

During recovery mode, the display should show:

- device id
- recovery AP SSID
- recovery AP IP address
- whether the device is waiting for setup or applying new settings
