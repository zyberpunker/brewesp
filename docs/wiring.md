# Wiring guide

This document defines the recommended pinout for the first hardware build.

The controller must still support headless operation, so none of the local UI
parts are mandatory for the first power-up.

## Recommended pin assignments

### Temperature probe

Use a 3-pin `DS18B20` style OneWire probe.

- `DATA`: `GPIO4`
- `VCC`: `3.3V`
- `GND`: `GND`

Important:

- add a `4.7k` pull-up resistor between `DATA` and `3.3V`
- keep all temperature probes on the same OneWire data pin if more are added
  later

### OLED display

Use a `128x64` OLED based on `SSD1306` or `SH1106`.

- `SDA`: `GPIO21`
- `SCL`: `GPIO22`
- `VCC`: `3.3V`
- `GND`: `GND`

### Buttons

Use four momentary buttons wired as active-low.

- `Up`: `GPIO32`
- `Down`: `GPIO33`
- `Select`: `GPIO25`
- `Back`: `GPIO26`

Recommended button wiring:

- one side of each button to the GPIO pin
- the other side of each button to `GND`
- use internal pull-ups in firmware

## Summary table

| Function | Pin |
|---|---|
| Temp probe data | `GPIO4` |
| OLED SDA | `GPIO21` |
| OLED SCL | `GPIO22` |
| Button Up | `GPIO32` |
| Button Down | `GPIO33` |
| Button Select | `GPIO25` |
| Button Back | `GPIO26` |

## Headless mode

If you have not yet connected:

- temperature probe
- display
- buttons

the controller can still boot and run in headless mode for provisioning,
network testing, MQTT testing, and relay integration work.

## Notes

- these assignments are the recommended defaults for documentation and initial
  firmware assumptions
- the project still keeps room for future config overrides where appropriate
