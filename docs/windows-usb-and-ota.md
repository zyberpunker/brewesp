# Flashing from Windows over USB-C and later OTA updates

This document explains:

1. how to flash the ESP32 from a Windows computer over USB-C
2. how OTA updates are intended to work in this project

The project currently has a firmware skeleton and OTA design, but OTA is not yet
fully implemented in code. The USB flashing steps are the primary path right now.

## Current assumptions

- host computer: Windows
- firmware workspace: `firmware/`
- current ESP connection: `COM4`
- firmware toolchain: PlatformIO with Arduino framework

## Part 1: First flashing over USB-C on Windows

### 1. Install the tooling

Recommended path:

- install Visual Studio Code
- install the PlatformIO IDE extension in VS Code

Reason:

- PlatformIO Core is included in PlatformIO IDE
- you can use the integrated terminal for `pio` commands

Alternative:

- install PlatformIO Core separately and enable shell commands on Windows

Useful official docs:

- PlatformIO installation: https://docs.platformio.org/en/latest/core/installation/index.html
- PlatformIO Core overview: https://docs.platformio.org/en/latest/core/

### 2. Connect the ESP32 over USB-C

- connect the board to the Windows computer with a data-capable USB-C cable
- wait for Windows to enumerate the serial device
- confirm the COM port in Device Manager

In your current setup, the board is already visible as `COM4`.

If Windows does not create a COM port:

- check whether the USB cable supports data, not only charging
- install the correct USB-to-UART driver for the board if needed
- reconnect the board and re-check Device Manager

The exact Windows driver depends on the USB bridge chip used by the board, which
varies between ESP32 development boards.

### 3. Open the firmware project

Open the repository in VS Code and use the firmware directory as the PlatformIO
project:

```powershell
cd C:\Users\ola\git\brewesp\firmware
```

The current PlatformIO config is in:

- [platformio.ini](C:\Users\ola\git\brewesp\firmware\platformio.ini)

### 4. Build the firmware

In the PlatformIO terminal:

```powershell
pio run
```

### 5. Upload firmware over USB

Use the detected COM port:

```powershell
pio run -t upload --upload-port COM4
```

If the COM port changes, replace `COM4` with the new port.

### 6. Open serial monitor

After flashing, open the serial monitor:

```powershell
pio device monitor --port COM4 --baud 115200
```

This should show boot logs from the firmware.

### 7. If upload fails

Common recovery actions:

- press and hold the board `BOOT` button
- tap `EN` or `RESET`
- release `BOOT` when flashing starts

This depends on the board, but it is a common manual fallback if automatic boot
into download mode does not work.

## Typical Windows workflow

```powershell
cd C:\Users\ola\git\brewesp\firmware
pio run
pio run -t upload --upload-port COM4
pio device monitor --port COM4 --baud 115200
```

## Part 2: How OTA should work in this project

The project should use this OTA model:

- initial flashing happens over USB-C
- later firmware updates happen over Wi-Fi
- MQTT is used to trigger update checks or update start
- firmware binaries are downloaded over HTTP/HTTPS

This matches the project architecture and avoids sending firmware binaries over
MQTT.

Related project docs:

- [architecture.md](C:\Users\ola\git\brewesp\docs\architecture.md)
- [mqtt-contract.md](C:\Users\ola\git\brewesp\docs\mqtt-contract.md)
- [system-config.schema.json](C:\Users\ola\git\brewesp\docs\schemas\system-config.schema.json)

### OTA prerequisites

For ESP32 OTA, the firmware layout must support at least:

- two OTA app slots, for example `ota_0` and `ota_1`
- an OTA data partition

Espressif documents this explicitly in the OTA docs.

Official docs:

- ESP-IDF OTA overview: https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/system/ota.html
- ESP HTTPS OTA: https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/system/esp_https_ota.html

### Recommended OTA flow for this project

1. Flash the first firmware over USB-C.
2. Configure Wi-Fi and MQTT.
3. Device comes online and reports its current firmware version.
4. Web service exposes a firmware manifest and a `.bin` download URL.
5. Web service or operator sends an MQTT command such as `check_update` or `start_update`.
6. ESP32 downloads the new firmware over HTTP/HTTPS.
7. ESP32 writes the image into the inactive OTA slot.
8. ESP32 reboots into the new image.
9. ESP32 reports success or rollback/failure via MQTT state/event topics.

### Why OTA should not send the binary over MQTT

- firmware binaries are large compared to normal MQTT messages
- HTTP/HTTPS is better suited to downloads, retries, and hosting
- it is easier to inspect, cache, and secure firmware artifacts

### Planned configuration for OTA

The current `system_config` schema already reserves an `ota` block with fields
such as:

- `enabled`
- `channel`
- `check_strategy`
- `check_interval_s`
- `manifest_url`
- `ca_cert_fingerprint`
- `allow_http`

See:

- [system-config.schema.json](C:\Users\ola\git\brewesp\docs\schemas\system-config.schema.json)

### Planned MQTT commands for OTA

The current MQTT contract reserves OTA-related commands such as:

- `check_update`
- `start_update`

and state fields such as:

- `fw_version`
- `ota_status`

See:

- [mqtt-contract.md](C:\Users\ola\git\brewesp\docs\mqtt-contract.md)

## Suggested practical rollout

Use this sequence:

1. Get USB flashing stable on Windows.
2. Verify serial logs and Wi-Fi provisioning.
3. Add custom partition table for OTA.
4. Implement OTA manager in firmware.
5. Host firmware manifest and binaries in the web service.
6. Add MQTT command path for OTA start/check.
7. Test upgrade and rollback on a spare firmware image before relying on it.

## Current project status

Today:

- USB flashing workflow is the way to install firmware
- OTA is designed in the docs but not completed in firmware yet

That means the first time you load new firmware, you should expect to do it over
USB-C from Windows.
