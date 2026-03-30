# Flashing from Windows over USB-C and later OTA updates

This document explains:

1. how to flash the ESP32 from a Windows computer over USB-C
2. how OTA updates are intended to work in this project

The repo now contains an OTA implementation path in firmware and web hosting,
but first install on a blank ESP32 is still done over USB-C.

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

Current repo implementation:

- `firmware/platformio.ini` uses `partitions_ota.csv`
- the partition table defines `ota_0`, `ota_1`, and `otadata`

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

Current repo wiring:

- manifest endpoint: `/firmware/manifest/<channel>.json`
- binary endpoint: `/firmware/files/<filename>`
- Docker Compose mounts `firmware/.pio/build/esp32dev` into the web container at
  `/app/firmware-files`
- the default hosted filename is `firmware.bin`
- scheduled OTA checks can run locally from saved config

### Why OTA should not send the binary over MQTT

- firmware binaries are large compared to normal MQTT messages
- HTTP/HTTPS is better suited to downloads, retries, and hosting
- it is easier to inspect, cache, and secure firmware artifacts

### OTA configuration in the repo

The current `system_config` schema and firmware config store support an `ota`
block with fields such as:

- `enabled`
- `channel`
- `check_strategy`
- `check_interval_s`
- `manifest_url`
- `ca_cert_fingerprint`
- `allow_http`

Current behavior:

- HTTPS OTA requires `ca_cert_fingerprint`
- HTTP OTA is rejected unless `allow_http` is enabled
- provisioning/recovery UI exposes the OTA fields and persists them locally

See:

- [system-config.schema.json](C:\Users\ola\git\brewesp\docs\schemas\system-config.schema.json)

### MQTT commands and state for OTA

The current MQTT contract and firmware command handler support OTA commands such
as:

- `check_update`
- `start_update`

and state fields such as:

- `fw_version`
- `ota_status`
- `ota_channel`
- `ota_available`
- `ota_progress_pct`
- `ota_target_version`
- `ota_message`
- `ota_reboot_pending`

and OTA-related events such as:

- `ota_check_completed`
- `ota_update_completed`

See:

- [mqtt-contract.md](C:\Users\ola\git\brewesp\docs\mqtt-contract.md)

## Suggested practical rollout

Use this sequence:

1. Get USB flashing stable on Windows.
2. Verify serial logs and Wi-Fi provisioning.
3. Build the firmware so `firmware/.pio/build/esp32dev/firmware.bin` exists.
4. Bring up the web service so it can host the mounted firmware artifact.
5. Set OTA config on the device, including `manifest_url` and, for HTTPS,
   `ca_cert_fingerprint`.
6. Trigger `check_update` or `start_update` over MQTT.
7. Test upgrade and failure handling on a spare device before relying on it.

## Current project status

Today:

- USB flashing workflow is still the first-install path
- OTA is implemented in firmware and web hosting in the repo
- real-device OTA validation is still recommended before relying on it in production

That means the first time you load new firmware, you should expect to do it over
USB-C from Windows.
