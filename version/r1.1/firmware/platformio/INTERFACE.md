# MiniSpeedCam — ESP32 ↔ STM32 Interface (HW rev 1.1)

This document describes how the two microcontrollers in the MiniSpeedCam
cooperate and the exact electrical + protocol contract between them. It covers
the PlatformIO firmwares in this folder:

- [`stm32_firmware_platformio/`](stm32_firmware_platformio) — STM32F301K8U6, the radar DSP.
- [`esp32_firmware_platformio/`](esp32_firmware_platformio) — ESP32-S3, the application controller.

## Roles

| MCU | Part | Responsibility |
|-----|------|----------------|
| **STM32** | STM32F301K8U6 (Cortex-M4F, 64 KB Flash / 16 KB RAM) | Samples the CDM324 Doppler radar's analog output, runs an FFT, and reports the detected speed. Nothing else — no WiFi, no camera. |
| **ESP32** | ESP32-S3-WROOM-1U | Master controller: polls the STM32 for speed, drives the OV2640 camera, hosts the ESPUI web config, manages WiFi/sleep, and uploads photos + speed to the cloud. |

The STM32 is a **slave "speed oracle."** The ESP32 is the master: it resets the
STM32, polls it, and acts on the results.

## Physical block diagram

```
        CDM324 radar (24.125 GHz Doppler)
                 │ analog IF
                 ▼
        ┌──────────────────┐                       ┌──────────────────┐
        │     STM32F301     │                       │     ESP32-S3      │
        │  (radar DSP)      │                       │  (app controller) │
        │                   │                       │                   │
 PB0 ─► │ ADC1_IN11 DOPPLER │                       │                   │
        │                   │   UART1 @ 1 Mbaud 8N1 │                   │
    PA9 │ USART1_TX ────────┼──────────────────────►│ GPIO42  RX        │
   PA10 │ USART1_RX ◄───────┼───────────────────────┤ GPIO41  TX        │
        │                   │                       │                   │
    PA5 │ GPIO_OUT ─────────┼──────────────────────►│ GPIO1   (wake/    │
        │ (motion flag)     │   motion-detected     │          activity)│
        │                   │                       │                   │
   NRST │ ◄─────────────────┼───────────────────────┤ GPIO47  (reset)   │
        │                   │   active-low reset    │                   │
    PA4 │ GPIO_IN  ◄────────┼───────────────────────┤ GPIO2   (radar    │
        │ (radar blank)     │   WiFi-TX blank       │          blank)   │
        └──────────────────┘                        └──────────────────┘
```

## Signal table

| Signal | STM32 pin | ESP32 pin | Dir | Notes |
|--------|-----------|-----------|-----|-------|
| Speed UART TX | `PA9` (USART1_TX) | `GPIO42` (RX) | STM → ESP | 1,000,000 baud, 8N1. Speed replies + boot banner. |
| Speed UART RX | `PA10` (USART1_RX) | `GPIO41` (TX) | ESP → STM | Single-byte query commands. |
| Motion / wake flag | `PA5` (GPIO out) | `GPIO1` (`ESP_WAKEUP_PIN`, in) | STM → ESP | HIGH = a valid (non-zero) speed is being reported; LOW = no motion. ESP firmware interprets this as "~≥5 mph motion." |
| STM32 reset | `NRST` | `GPIO47` (`STM32_RESET_PIN`, out) | ESP → STM | Active-low. ESP pulses low ~20 ms to reset the radar MCU. |
| Radar blank | `PA4` (in, pull-down) | `GPIO2` (`RADAR_BLANK_PIN`, out) | ESP → STM | ESP drives **HIGH** while transmitting a WiFi burst (upload / (re)connect); the STM32 **discards** any FFT frame seen while it's high. Was the reserved `STM_WAKEUP` line. |
| Debug UART (unused by link) | `PA2`/`PA3` (USART2) | — | — | USART2 @ 115200 is initialized on the STM32 but not part of the ESP link (debug header). |

## UART protocol (USART1, 1 Mbaud, 8N1)

Strictly master-polled request/response. The ESP sends one ASCII byte; the STM32
replies with one **checksummed** ASCII line terminated by `\r\n`.

| ESP sends | STM32 replies | Meaning |
|-----------|---------------|---------|
| `'m'` | `<int>*<CK>\r\n` | Latest speed in **MPH × 10**, checksummed |
| `'k'` | `<int>*<CK>\r\n` | Latest speed in **KPH × 10**, checksummed |
| `'a'` / `'s'` | (raw dump on/off) | Debug: stream raw ADC/FFT buffers (not used in normal operation) |
| `'h'` / `'l'` | — | Debug: enable/disable low-frequency rejection |
| `'d'` | — | Debug: toggle the per-frame `DBG …` diagnostic dump (emitted on **USART2**, not this link) |

- **Reply format:** `<int>*<CK>\r\n`, e.g. `298*33`. `<int>` is **speed × 10**
  (so `298` → 29.8). `<CK>` is the **XOR of the value's ASCII digits**, printed as
  two hex chars (`'2'^'9'^'8' = 0x33`). The ESP verifies the checksum in
  [`get_speed()`](esp32_firmware_platformio/src/radar.h) and returns 0 on any mismatch.
  This rejects the dangerous corruption the plausibility ceiling can't catch — a
  flipped bit that lands on another in-range value (e.g. `29` → `79` mph). The
  STM32 derives `<int>` from the FFT peak frequency using a CDM324 calibration
  constant (≈0.145 for MPH×10, ≈0.226 for KPH×10) — see the command handler /
  `uart_reply_speed()` in [STM32 `main.c`](stm32_firmware_platformio/Core/Src/main.c).

### Important timing/behavior contract

- **The STM32 only answers when it has a fresh sample.** UART commands are serviced
  *inside* the "FFT just completed" branch of the STM32 main loop, and the motion
  flag (`PA5`) is updated only every `FFT_CNT_DISP` (10) completed FFTs. If the ESP
  polls between samples, **there may be no reply.**
- Because of this, the ESP's `get_speed()` waits up to **50 ms** for a reply and
  returns **0** (treated as "no motion") if nothing arrives — it never blocks the
  radar loop on a missing reply.
- The ESP **drains stale RX bytes before each query**, **verifies the reply
  checksum**, and **rejects implausible readings** (negative, or above
  `MAX_PLAUSIBLE_SPEED = 250`) as line noise. RF noise — often from the ESP's own
  WiFi bursts — can inject junk on the UART line; these guards keep it out of the
  trigger/upload path. (This hardening is specific to the PlatformIO firmware; the
  original Arduino sketch lacked it.)
- On the STM32 side, command bytes are received **by interrupt into a ring buffer**
  with a `HAL_UART_ErrorCallback` that clears overrun/framing errors and re-arms —
  so a noise byte during the ~2.4 ms FFT can't overrun the FIFO-less USART and
  permanently deafen the link. An **independent watchdog (IWDG, ~2 s)** resets the
  MCU if the radar loop ever wedges. Neither affects the wire protocol.

## GPIO signaling

### Motion flag — `PA5` → `GPIO1`
A fast hardware "is anything moving?" line, independent of the UART poll. The STM32
drives it **HIGH** whenever the currently reported speed is non-zero (valid), and
**LOW** when no motion is detected. The ESP reads it to gate sleep and
WiFi-reconnect decisions. (Hardware light-sleep wake-on-this-pin is wired but
commented out in rev 1.1, since USB would disconnect on sleep.)

### Radar blank — `GPIO2` (`RADAR_BLANK_PIN`) → `PA4`
A WiFi-TX blanking handshake that attacks the rev-1.1 noise problem at its source.
The ESP32 and CDM324 share a supply rail, so the ESP's WiFi TX current bursts slump
the rail and corrupt the radar reads. The ESP drives this line **HIGH** for the
duration of each WiFi burst it controls — HTTPS uploads (`sendUpload`/`sendLocalIP`)
and WiFi (re)connects (`connectWifi`/`connectWifiAP`) — via a depth-counted
`RadarBlankGuard` so nested calls keep it asserted until the outermost scope exits.
The STM32 reads `PA4` at the top of each FFT frame (`analog_compute_fft_on_cplted_sequence`)
and, when it's high, **discards that frame** (holds the previous median, skips the
FFT). This is a direct "this frame is contaminated" flag — more reliable than
inferring it from the noise statistics. `PA4` has an internal pull-down so it reads
"not blanking" while the ESP is booting / the line floats. The `DBG` diagnostic
dump on USART2 shows `blank=1` on a discarded frame. Was the reserved `STM_WAKEUP`
line.

### Reset — `GPIO47` → `NRST`
The ESP owns the STM32's reset. At boot it pulses `STM32_RESET_PIN` low for ~20 ms
(`issue_cdm324_reset()`), then drains the STM32's boot banner
(`"Connection Successful!"` / `"CDM324-V2 fw v…"`) so it doesn't corrupt the first
`parseFloat()` of a speed reply.

## End-to-end operational flow

1. **Boot:** ESP resets the STM32, drains its banner, joins WiFi (or starts a
   captive-portal AP for configuration), and loads user settings (units,
   `min_speed`, `photo_speed`).
2. **Idle polling:** On Core 1 the ESP polls the STM32 (~10 Hz) for speed and
   watches the motion flag.
3. **Vehicle detected** (`speed ≥ min_speed`): the ESP enters a tracking loop,
   recording the run's `maxSpeed`. When `maxSpeed ≥ photo_speed` it fires the
   OV2640 once.
4. **Vehicle clears** (`speed < min_speed`): the ESP hands off to Core 0, which
   POSTs the base64-encoded JPEG + max speed to the cloud. Core 0 is kept separate
   so the slow HTTPS upload never stalls radar polling on Core 1.

## Quick reference: changing the interface

- **Change the UART baud:** must match on both sides — STM32 `MX_USART1_UART_Init()`
  (`huart1.Init.BaudRate`) in `main.c` **and** `Serial1.begin(...)` in the ESP
  `setup()`.
- **Change a command/units:** edit the STM32 command handler (`'m'`/`'k'` cases) and
  the ESP `get_speed()` together — the `× 10` scaling and unit semantics are a
  shared contract.
- **Change the reply format / checksum:** the `<int>*<CK>\r\n` framing is a shared
  contract — update `uart_reply_speed()` in STM32 `main.c` **and** the line-parse +
  checksum verification in the ESP `get_speed()` together, or replies will be
  rejected as corrupt.
- **Change a pin:** update both the STM32 `.ioc`/`MX_GPIO_Init` (or USART pin
  remap) and the ESP `#define`s near the top of `main.cpp`.
