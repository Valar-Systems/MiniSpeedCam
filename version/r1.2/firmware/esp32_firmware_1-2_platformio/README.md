# MiniSpeedCam ESP32 Firmware (PlatformIO)

PlatformIO port of the Arduino sketch at
[../esp32_firmware_1-2/esp32_firmware_1-2.ino](../esp32_firmware_1-2/esp32_firmware_1-2.ino).

Functionality is identical; the only changes are project layout (a `src/` tree
with paired `.h`/`.cpp` modules and `extern` globals) so the project builds
under PlatformIO without relying on Arduino IDE's multi-tab `.ino`
preprocessor.

## Layout

```
esp32_firmware_1-2_platformio/
├── platformio.ini      Build environment (board, partitions, lib_deps)
└── src/
    ├── main.cpp        Arduino setup()/loop() + dual-core tasks
    ├── pins.h          GPIO assignments for board revision 1.2
    ├── variables.h     Shared state grouped into POD structs (see below)
    ├── variables.cpp   One-definition home for the struct instances
    ├── camera.{h,cpp}  OV2640 init + JPEG capture (base64-encoded)
    ├── radar.{h,cpp}   UART protocol with the STM32 radar MCU
    ├── api.{h,cpp}     WiFi bring-up + HTTPS uploads to minispeedcam.com
    └── espui_settings.{h,cpp}  Web-UI controls + NVS persistence
```

### Shared state

Globals live in `variables.h` as five small POD structs, each instantiated
once in `variables.cpp`. The Core 1 (radar) task writes the radar/sleep
side and raises flags on `upload`; the Core 0 (network) task drains them
and consumes `net` for WiFi.

| Instance     | Type           | Contents                                              |
|--------------|----------------|-------------------------------------------------------|
| `radar`      | `RadarState`   | `speed`, `maxSpeed`, `min_speed`, `photo_speed`, `is_kph` |
| `sleep_gate` | `SleepGate`    | post-boot grace + measurement blanking windows        |
| `upload`     | `UploadJob`    | Core 1 -> Core 0 flags + photo payload                |
| `net`        | `NetConfig`    | persisted WiFi creds + camera id + local IP           |
| `ui_ids`     | `EspuiHandles` | ESPUI control IDs for the credential text inputs      |
| `preferences`| `Preferences`  | NVS-backed key/value store                            |

## Build / flash

```sh
pio run                 # compile
pio run -t upload       # flash over USB
pio device monitor      # 115200 baud
```

## Board options

The defaults in `platformio.ini` target an **ESP32-S3-WROOM-1-N8R8**
(8 MB flash, 8 MB OPI PSRAM). For different modules edit:

| `platformio.ini` key              | Typical value           |
|-----------------------------------|-------------------------|
| `board_build.arduino.memory_type` | `qio_opi` (OPI PSRAM)   |
| `board_upload.flash_size`         | `8MB` / `16MB`          |
| `board_build.partitions`          | `huge_app.csv`          |

If the board uses the on-chip native USB rather than a USB/UART bridge,
uncomment the `ARDUINO_USB_MODE=1` and `ARDUINO_USB_CDC_ON_BOOT=1`
lines in `build_flags` so the serial console works without an external
adapter.

## Dependencies

Pulled automatically via `lib_deps`:

- `s00500/ESPUI @ 2.2.4` - web configuration portal
- `esp32async/AsyncTCP @ 3.3.8` - async networking (ESPUI dependency)

`esp_camera`, `WiFi`, `WiFiClientSecure`, `HTTPClient`, `Preferences`,
`ESPmDNS`, and `DNSServer` are provided by the `espressif32` platform's
Arduino framework and need no entry.

## Photo upload path

The speeding-event photo POST never materialises the encoded JPEG in
regular heap. Capture leaves the JPEG in the camera driver's PSRAM
framebuffer; an opaque pointer lives in an atomic slot owned by
[camera.cpp](src/camera.cpp). The Core 0 uploader feeds the framebuffer
into a `StreamingUploadBody` (in [api.cpp](src/api.cpp)) that emits the
JSON prologue, on-the-fly base64 chunks of the JPEG, and the JSON
epilogue as `HTTPClient::sendRequest()` pulls ~1.4 KB at a time into
its TCP write buffer. Peak heap during upload is therefore dominated
by the TCP buffer plus a couple of short Strings, not the photo.
