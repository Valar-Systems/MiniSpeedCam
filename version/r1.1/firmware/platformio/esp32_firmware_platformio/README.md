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
    ├── diagnostics.h       Reset reason / boot count / uptime / last-upload code
    ├── camera_stream.h/.cpp  Temporary MJPEG "aiming" stream (esp_http_server on :81)
    └── config_portal.h     Self-hosted web config page (esp_http_server on :80) + JSON API + NVS persistence
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

- `bblanchon/ArduinoJson @ ^7.2.0` - config portal `/api/state` JSON + POST bodies, and the OTA manifest

`esp_camera`, `esp_http_server` (both HTTP servers — config portal :80 and
aiming stream :81), `WiFi`, `WiFiClientSecure`, `HTTPClient`, `Preferences`,
`ESPmDNS`, `DNSServer`, and `SPIFFS` are provided by the `espressif32`
platform's Arduino framework and need no entry. The JPEG is base64-encoded
with the core's `base64::encode()` (`cores/esp32/base64.h`), so no base64
library is required either. There is no ESPUI / ESPAsyncWebServer / AsyncTCP
dependency — the config page is hand-rolled HTML served from flash.

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

## Web UI

Self-hosted config page at the device's IP (`MiniSpeedCam.local` over mDNS),
served from flash by `esp_http_server` on port 80 (`config_portal.h`):

- **Device** — live Current Speed readout, MPH/KPH switch, Power-Saver Mode
  switch (drop WiFi when idle vs. never sleep), minimum speed, photo speed, and
  the proximity/photo-signal thresholds.
- **Aiming stream** — toggle the temporary MJPEG video feed (port 81) for
  mounting.
- **WiFi & data server** — Network SSID / password, Data Server API base URL,
  Save (reboots), Clear Settings (reboots into the setup AP).
- **Status** — live diagnostics: signal (proximity), free heap and PSRAM,
  uptime, WiFi RSSI and IP, last upload HTTP code, last reset reason, boot
  count, firmware versions, and OTA status.

There is no server-side push: the page polls `GET /api/state` (~1 Hz) for all
live values and writes settings back via small JSON `POST` endpoints
(`/api/settings`, `/api/wifi`, `/api/clear`, `/api/stream`). That replaced the
old ESPUI WebSocket broadcast from `taskCore1`, whose AsyncTCP send blocked the
radar task and froze the device under WiFi/stream load.

### `GET /api/events` — recent detections for LAN companions

A read-only feed of the most recent passes, so a display on the same network can
show recent speeds without the cloud (`events.h`, an in-RAM ring written at
run-end). The reference consumer is the Blipscope **Speedscope** edition. Shape:

```json
{ "kph": false, "count": 2,
  "events": [ { "speed": 31, "ageSec": 4, "mag": 1200, "dir": 1 },
              { "speed": 24, "ageSec": 47, "mag": 780, "dir": 2 } ] }
```

Newest first; `speed` is a whole number in the device's configured unit (`kph`
flag), `ageSec` is seconds since the pass (this board has no RTC — use it with
your own clock for absolute times), and `dir` is `0` unknown / `1` approaching /
`2` receding. CORS-open (`Access-Control-Allow-Origin: *`).

## Power management

**Power-Saver Mode** (Device tab, persisted in NVS, default **on**) gates the
radar loop's two idle paths: after the post-boot grace window, and after 5 s of
zero radar activity, the device drops WiFi (`WiFi.disconnect` + `WIFI_OFF`).
Turn it **off** to keep the device permanently awake with WiFi up (e.g. while
configuring or debugging) — toggling it off also triggers an immediate
reconnect so the web UI comes straight back. `esp_light_sleep_start()` is still
commented out in `taskCore1`, so today the saving is WiFi-off only.

## TODO

### Verify before relying on this build
- [ ] Hardware smoke-test the capture/upload path: confirm a speeding pass
      produces a well-formed streamed photo POST, and a pass between
      `min_speed` and `photo_speed` produces a valid speed-only POST.
- [ ] Confirm the `capture` endpoint accepts speed-only events (no `photo`
      object). Behaviour changed: **every** pass now uploads, not just speeding
      ones — gate the enqueue on `has_photo` in `taskCore1` if that's unwanted.

### Open decisions / known gaps
- [ ] Captive DNS is never started — `dnsServer.processNextRequest()` runs but
      `dnsServer.start(DNS_PORT, "*", apIP)` is never called, so the AP-mode
      portal redirect is inactive. Wire it up in `connectWifiAP()` or remove the
      unused `DNS_PORT`/`apIP`.
- [ ] Post-boot grace window: code uses `millis() + 10000` (10 s) while comments
      say 120 s — pick one and align code + comments.
- [ ] `speed`/`maxSpeed` are `int`, so `get_speed()`'s float is truncated.
      Decide whether sub-unit precision should be sent (server-contract change).
- [ ] Confirm `WIFI_RESET_PIN` needs the internal pull-up (now `INPUT_PULLUP`)
      vs. an external one on the board.

### Security
- [ ] Rotate the bearer token (it exists in git history) and/or supply it only
      via the `API_BEARER_TOKEN` build flag.
- [ ] Enable TLS validation by setting `API_CA_ROOT_CERT` (currently falls back
      to `setInsecure()`).

### Possible backports from r1.2
- [ ] OTA updates (ArduinoOTA + dual-app `default_8MB.csv` partition; pause the
      camera during a transfer).
- [x] Power-Saver Mode switch — toggle WiFi-drop on idle vs. never sleep
      (persisted, with immediate reconnect on disable). *(done)*
- [ ] Power management, deeper: re-enable `esp_light_sleep_start()` and
      `esp_camera_deinit()` on idle (still commented out in `taskCore1`), gated
      by the same Power-Saver Mode flag.
- [ ] Offline event queue (LittleFS) with retry, so a capture survives a failed
      upload / WiFi outage instead of being lost.
- [ ] UI-tunable camera (frame size / JPEG quality) in the Device tab.
- [x] Live diagnostics Status tab (heap, PSRAM, uptime, RSSI, IP, last upload,
      reset reason, boot count). *(done)*

### Maintenance
- [ ] Decide whether to port these fixes back into the original Arduino-IDE
      sketch at `../esp32_firmware/esp32_firmware.ino`.
