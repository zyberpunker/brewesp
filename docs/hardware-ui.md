# Hardware and local UI decisions

This document captures the current hardware direction for the first build.

## Chosen controller hardware

Use the existing ESP32 board already available in the project context:

- chip: `ESP32-D0WD-V3`
- dual core
- 240 MHz
- 4 MB flash

Reason:

- enough performance for control loop, MQTT, OTA, and local UI
- no need to switch board just to get I2C
- lower project risk than changing MCU platform now

## Local UI direction

Use:

- external `128x64` monochrome OLED
- `I2C` connection
- `4` physical buttons

Recommended local functions:

- `Up`: increase setpoint or move up in menu
- `Down`: decrease setpoint or move down in menu
- `Select`: confirm or activate selected action
- `Back`: close menu, go back, or acknowledge alarm

The local UI should be an operator panel, not the primary configuration system.
The web service remains the main place for full settings and profiles.

The controller must also support running without any local panel hardware.

Headless mode requirements:

- controller starts and runs normally without display connected
- controller starts and runs normally without buttons connected
- local UI features are enabled only when configured in `system_config`
- all critical control and configuration paths remain available through network
  interfaces

## Recommended display type

First choice:

- `SSD1306` or `SH1106` based `128x64` OLED over I2C at `3.3V`

Reason:

- widely supported on ESP32
- low pin count
- enough resolution for temperatures, state, Wi-Fi, MQTT, alarms
- simple and robust for a controller appliance
- display sleep behavior can reduce OLED burn-in risk

## Suggested wiring model

These are proposed defaults, not hard requirements.

- `SDA`: `GPIO21`
- `SCL`: `GPIO22`
- `BTN_UP`: `GPIO32`
- `BTN_DOWN`: `GPIO33`
- `BTN_SELECT`: `GPIO25`
- `BTN_BACK`: `GPIO26`

Notes:

- all buttons should use pull-up logic and be active-low
- exact pins should remain configurable in `system_config`
- final pin selection must avoid conflicts with boot strapping and board-specific
  constraints
- these pins are used only when local buttons are enabled
- recommended fixed wiring defaults are documented in [wiring.md](C:\Users\ola\git\brewesp\docs\wiring.md)

## UI screen priorities

### Home screen

- current primary temperature
- current setpoint
- active mode
- heating/cooling status
- Wi-Fi and MQTT status
- fault/alarm indicator

### Quick actions

- adjust setpoint up/down
- pause/resume active profile
- disable heating
- disable cooling

### Diagnostics

- firmware version
- uptime
- IP address
- RSSI
- output backend summary

## Locked v1 local UI behavior

This section defines the first implementation target for the OLED and buttons.

### Screens in v1

Use only two screens in the first version:

1. `home`
2. `diagnostics`

Any other actions should be done through a simple menu overlay, not full extra
screens.

### Home screen contents

The home screen should show:

- primary temperature
- secondary temperature if available
- active setpoint
- active mode: `thermostat` or `profile`
- output state: `heating`, `cooling`, or `idle`
- Wi-Fi status
- MQTT status
- fault or alarm marker

### Display sleep behavior in v1

The OLED should not stay fully lit forever.

Default behavior:

- dim after `30` seconds of inactivity
- turn display off after `120` seconds of inactivity
- any button press wakes the display

To avoid accidental actions:

- if the display is off, the first button press wakes it
- that wake press should not change setpoint or trigger menu actions
- the next press performs the intended action

Automatic wake events:

- new alarm or fault
- Wi-Fi disconnect
- MQTT disconnect

Display should stay awake while:

- the menu is open
- the diagnostics screen is open
- the user is actively changing setpoint
- an unacknowledged fault is on screen

### Button behavior in v1

#### `Up`

- short press on `home`: increase active setpoint
- short press in menu: move selection up
- short press in `diagnostics`: scroll up if needed

#### `Down`

- short press on `home`: decrease active setpoint
- short press in menu: move selection down
- short press in `diagnostics`: scroll down if needed

#### `Select`

- short press on `home`: open menu
- short press in menu: activate selected item
- short press in `diagnostics`: no action

#### `Back`

- short press on `home`: acknowledge active alarm if present
- short press in menu: close menu and return to `home`
- short press in `diagnostics`: return to `home`
- long press anywhere: return to `home`

### v1 menu items

Keep the menu intentionally small.

Menu entries:

1. `Pause/Resume Profile`
2. `Disable Heating`
3. `Disable Cooling`
4. `Diagnostics`
5. `Back`

### v1 local setpoint semantics

Local setpoint changes on the home screen should:

- update the active setpoint immediately on the device
- be treated as a local override of the running control target
- be published in device state so the web service can display it

The web service should later decide whether to persist that override into the
main fermentation configuration.

### v1 profile behavior

The local panel may:

- pause an active profile
- resume a paused profile

The local panel should not in v1:

- edit profile steps
- create profiles
- change ramping behavior

### v1 output disable semantics

`Disable Heating` and `Disable Cooling` are local operator overrides.

Behavior:

- disabling heating blocks automatic heating requests
- disabling cooling blocks automatic cooling requests
- disabling one output must not affect the other
- these overrides must be visible in MQTT state and on the home screen
- these overrides remain subject to sensor-fault shutdown and other safety rules

### v1 diagnostics screen

The diagnostics screen should show:

- firmware version
- uptime
- IP address
- Wi-Fi RSSI
- MQTT broker connected/disconnected
- output backend type for heating and cooling

### Deferred to later versions

Do not include these in the first local UI implementation:

- local profile editing
- local Wi-Fi provisioning
- local MQTT broker editing
- OTA initiation from the local screen
- historical graphs on-device
- touch-style nested menus

## Safety behavior for local controls

Local controls must not bypass controller safety rules.

That means:

- never force heating and cooling on together
- still respect minimum heating/cooling delay timers
- still shut outputs down on sensor fault
- local disable of heating/cooling should override automatic requests

## MVP scope for local UI

Phase 1 local UI should support:

- show temperatures and current state
- raise/lower active setpoint
- show Wi-Fi/MQTT connectivity
- show faults
- support four-button navigation: `Up`, `Down`, `Select`, `Back`

Phase 2 can add:

- profile pause/resume
- menu navigation
- local network diagnostics
- local onboarding helpers

## Provisioning and recovery interaction

The local panel should support recovery, but it is not the primary provisioning
surface.

Recommended behavior:

- first-time setup happens through the ESP32 recovery AP and web page
- the local OLED should show that setup mode is active
- the OLED should display the recovery SSID and IP address
- a boot-time button hold should be able to force recovery mode

Suggested manual recovery:

- hold `Back` during boot to enter recovery AP mode

During recovery mode, the display should show:

- device id
- recovery AP SSID
- recovery AP IP address
- whether the device is waiting for setup or applying new settings
