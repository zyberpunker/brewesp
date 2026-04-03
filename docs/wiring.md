# Wiring guide

This document defines the recommended pinout for the current hardware plan:

- existing `ESP32-D0WD-V3` board
- `AZ-Touch MOD` 2.4" display with resistive touch
- `2 x DS18B20` probes on one shared OneWire bus

The controller must still support headless operation, so none of the local UI
parts are mandatory for first power-up.

## AZ-Touch internal pin usage

For the `AZ-Touch MOD` with ESP32, the commonly documented internal wiring is:

| AZ-Touch function | ESP32 pin |
|---|---|
| Display MOSI | `GPIO23` |
| Display MISO | `GPIO19` |
| Display SCK | `GPIO18` |
| Display CS | `GPIO5` |
| Display D/C | `GPIO4` |
| Display RESET | `GPIO22` |
| Backlight PWM | `GPIO15` |
| Touch CS | `GPIO14` |
| Touch IRQ | `GPIO27` |
| Touch shares MOSI/MISO/SCK | `GPIO23` / `GPIO19` / `GPIO18` |

Sources:

- [AZ-Touch MOD openHASP](https://www.openhasp.com/0.7.0/hardware/az-delivery/az-touch/)
- [AZ-Delivery AZ-Touch ESP32 wiring article](https://www.az-delivery.uk/es/blogs/azdelivery-blog-fur-arduino-und-raspberry-pi/terminanzeige-mit-az-touch-mod-am-esp32-mqtt-node-red-und-esphome-teil-3)

Important consequence:

- the old `GPIO4` plan for the DS18B20 bus must not be used with AZ-Touch,
  because `GPIO4` is already wired to the display `D/C` line

## Recommended pin assignments for BrewESP

### Temperature probes

Use two 3-pin `DS18B20` style probes on one shared OneWire bus.

- `DATA bus`: `GPIO32`
- `VCC`: `3.3V`
- `GND`: `GND`

Probe roles:

1. `beer probe`
   attach to fermenter/thermowell and use as primary control probe
2. `chamber probe`
   place in the chamber air and use as secondary diagnostics/limiting probe

Important:

- add one `4.7k` pull-up resistor between `GPIO32` and `3.3V`
- connect both probe data wires to the same `GPIO32`
- identify each DS18B20 by ROM address in firmware and map it to `beer` or
  `chamber`
- avoid parasitic-power wiring; use proper `VCC`, `DATA`, `GND` for both probes

## First-time probe mapping workflow

When the probes are connected for the first time:

1. Connect only the `beer probe`.
2. Boot BrewESP and open the device page in the web UI.
3. Read the `Beer probe ROM` value from the device detail sensor status view.
4. Disconnect power and connect the `chamber probe` as well.
5. Boot again and read the `Chamber probe ROM` value from the same view.
6. Store these ROMs in `system_config` later so the mapping stays deterministic
   even if probes are swapped or one probe is temporarily missing.

Current firmware fallback behavior before ROMs are pinned:

- first detected DS18B20 becomes `beer`
- second detected DS18B20 becomes `chamber`

That is fine for bring-up, but explicit ROM mapping is the correct end state.

### Optional service/recovery button

Touch is the normal local input method, but a hidden service button is still
recommended.

- `SERVICE_BTN`: `GPIO33`

Suggested use:

- hold during boot to force recovery AP mode

### Optional buzzer

The AZ-Touch board includes a buzzer on the panel PCB. Firmware support can be
added later once the exact board revision and pin assignment are verified in the
final assembled hardware.

## Summary table

| Function | Pin |
|---|---|
| DS18B20 data bus | `GPIO32` |
| Service / recovery button | `GPIO33` |
| AZ-Touch display/touch | uses fixed internal wiring listed above |

## Headless mode

If you have not yet connected:

- temperature probes
- touchscreen panel
- service button

the controller can still boot and run in headless mode for provisioning,
network testing, MQTT testing, and relay integration work.

## Notes

- these assignments are the recommended defaults for documentation and initial
  firmware assumptions
- the project should treat the beer probe as the primary control sensor
- the chamber probe should be available from the start, even if the first
  firmware revision uses it mainly for visibility and diagnostics
