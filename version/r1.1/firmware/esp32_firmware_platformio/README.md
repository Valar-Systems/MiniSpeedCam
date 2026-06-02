# MiniSpeedCam ESP32 Firmware r1.1 (PlatformIO)

PlatformIO port of the Arduino sketch at
[../esp32_firmware/esp32_firmware.ino](../esp32_firmware/esp32_firmware.ino).

Functionality is identical to the original sketch. The only change is project
layout: the sketch body becomes `src/main.cpp` and the original multi-tab
header modules move under `src/` unchanged. They are still compiled as a
single translation unit (`main.cpp` `#include`s them in the same order the
Arduino IDE concatenated the `.ino` tabs), so there is no behavioural
difference — this is purely so the project builds under PlatformIO without
relying on the Arduino IDE's `.ino` preprocessor.

> For the newer, fully-modularised layout (paired `.h`/`.cpp`, `extern`
> globals, OTA, offline queue, diagnostics) see
> [../../../r1.2/firmware/esp32_firmware_1-2_platformio](../../../r1.2/firmware/esp32_firmware_1-2_platformio).

## Layout

```
esp32_firmware_platformio/
├── platformio.ini          Build environment (board, partitions, lib_deps)
└── src/
    ├── main.cpp            GPIO/pin defines + setup()/loop() + dual-core tasks
    ├── config.h            Bearer token + optional TLS root cert (build-flag overridable)
    ├── variables.h         Shared state + the Core 1 -> Core 0 upload queue
    ├── camera.h            OV2640 init + JPEG capture (framebuffer kept for streaming)
    ├── radar.h             UART protocol with the STM32 radar MCU
    ├── api.h               WiFi bring-up + streaming HTTPS upload to minispeedcam.com
    └── espui_settings.h    Web-UI controls + NVS persistence
```

## Build / flash

```sh
pio run                 # compile
pio run -t upload       # flash over USB
pio device monitor      # 115200 baud
```

## Board options

The defaults in `platformio.ini` target an **ESP32-S3-WROOM-1-N8R8**
(8 MB flash, 8 MB OPI PSRAM). For different modules edit:

| `platformio.ini` key              | Typical value             |
|-----------------------------------|---------------------------|
| `board_build.arduino.memory_type` | `qio_opi` (OPI PSRAM)     |
| `board_upload.flash_size`         | `8MB` / `16MB`            |
| `board_build.partitions`          | `huge_app.csv` (3 MB app) |

If the board uses the on-chip native USB rather than a USB/UART bridge,
uncomment the `ARDUINO_USB_MODE=1` and `ARDUINO_USB_CDC_ON_BOOT=1` lines in
`build_flags` so the serial console works without an external adapter.

## Dependencies

Pulled automatically via `lib_deps`:

- `s00500/ESPUI @ 2.2.4` - web configuration portal
- `esp32async/AsyncTCP @ 3.3.8` - async networking (ESPUI dependency)

`esp_camera`, `WiFi`, `WiFiClientSecure`, `HTTPClient`, `Preferences`,
`ESPmDNS`, `DNSServer`, and `SPIFFS` are provided by the `espressif32`
platform's Arduino framework and need no entry. The JPEG is base64-encoded
with the core's `base64::encode()` (`cores/esp32/base64.h`), so no base64
library is required either.

## Configuration

Deployment values live in [src/config.h](src/config.h) and can be overridden
at build time (so you don't have to edit source) via `build_flags` in
`platformio.ini`:

| Macro               | Default                          | Purpose                                         |
|---------------------|----------------------------------|-------------------------------------------------|
| `API_BEARER_TOKEN`  | the shared minispeedcam.com key  | `Authorization: Bearer` token for the uploads   |
| `API_CA_ROOT_CERT`  | empty                            | PEM root cert for TLS validation                |

```ini
build_flags =
    ...
    -DAPI_BEARER_TOKEN=\"my-token\"
```

**TLS:** with `API_CA_ROOT_CERT` empty, HTTPS uploads use `setInsecure()`
(no certificate validation — the original behaviour). Paste the PEM root that
signs `minispeedcam.com` into `config.h` (or pass it via the build flag) to
enable validation:

```sh
openssl s_client -connect minispeedcam.com:443 -showcerts </dev/null
```

The default token is a shared Bubble.io workflow key, not a per-device secret;
it already exists in this repo's git history, so rotate it server-side if it
ever needs to be private.

## Capture & upload path

Core 1 (radar) and Core 0 (network) hand work off through a FreeRTOS queue
(`uploadQueue`) rather than polled flags:

1. While a vehicle is in range (`speed >= min_speed`), Core 1 tracks the run's
   max speed and, once `maxSpeed >= photo_speed`, captures **one** JPEG and
   keeps its PSRAM framebuffer.
2. When the run ends, Core 1 posts a single `UploadRequest{speed_actual,
   has_photo, fb}` to the queue.
3. Core 0 blocks on the queue, then POSTs to `server_capture`. When a photo is
   attached it is base64-streamed straight from the framebuffer by
   `StreamingUploadBody` (an `Arduino Stream` feeding
   `HTTPClient::sendRequest`), so the ~200 KB encoded image is never buffered
   in heap. Core 0 then returns the framebuffer.

Every run uploads exactly once: a full photo POST when speeding, or a short
speed-only POST otherwise (the `send_photo` JSON field tells the server which).
The camera is configured with `fb_count = 2` so Core 1 can capture a new frame
while Core 0 is still streaming the previous one.
