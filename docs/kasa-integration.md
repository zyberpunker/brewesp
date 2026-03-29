# TP-Link Kasa integration notes

This document captures the current integration plan for TP-Link Kasa devices,
starting with `KP105`.

## Goal

Support `KP105` as an optional network output backend for:

- `heating`
- `cooling`

without making Kasa a hard dependency for the whole firmware design.

## Strategy

Do not try to port the full `python-kasa` project into the ESP32 firmware.

Instead:

1. use `python-kasa` as a protocol reference
2. implement a very small `kasa_local` driver in C++
3. support only the operations needed by the controller

First required operations:

- connect to the device on the local network
- query on/off state
- switch on
- switch off

That keeps the firmware smaller, easier to debug, and less coupled to Kasa
features we do not need for temperature control.

## Why this approach

`python-kasa` already supports `KP105`, so it is a practical reference for the
device family and local protocol behavior.

But the firmware only needs a tiny subset of that functionality:

- output state readback
- output on/off commands
- basic timeout/error handling

## Implementation boundaries

The `kasa_local` firmware driver should:

- live behind the common `OutputDriver` interface
- be optional in `system_config`
- be safe to disable or replace with `gpio` or `shelly_http_rpc`

The first implementation should not include:

- discovery
- scene/schedule support
- cloud integration
- advanced telemetry
- unsupported device families

## Validation plan

Validation must be done against the actual device in use:

- model: `KP105`
- exact firmware version on the plug
- local LAN only

Reason:

- Kasa device behavior can vary across firmware generations
- the controller should not assume that all Kasa devices behave identically

## Licensing and reuse

The preferred approach is to reuse protocol understanding and packet flow rather
than copy large parts of the Python implementation.

If code is reused directly, keep the upstream license and attribution requirements
in mind.
