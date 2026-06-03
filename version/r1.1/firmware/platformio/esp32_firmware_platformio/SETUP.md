# MiniSpeedCam — Setup & User Guide

MiniSpeedCam measures the speed of passing vehicles, takes a photo when a
vehicle exceeds the speed you choose, and uploads the photo and speed to your
account at **[minispeedcam.com](https://minispeedcam.com/)**, where you can
view and download the results.

This guide explains how the device works, how to set it up (including linking
it to your minispeedcam.com camera), and what every setting does.

---

## How it works

The device combines three parts:

1. **CDM324 Doppler radar** — outputs a small voltage proportional to a moving
   vehicle's speed. An amplifier circuit boosts that signal.
2. **STM32 microcontroller** — runs an FFT on the amplified radar signal to
   compute the actual speed, and reports it over a serial (UART) link.
3. **ESP32-S3** — the brain. It polls the STM32 for the current speed, hosts the
   configuration web page over WiFi, and when a vehicle's speed crosses your
   **Photo Speed** threshold it captures a JPEG from the camera and POSTs the
   photo + maximum speed to minispeedcam.com.

A typical pass looks like this:

```
Vehicle approaches
   │
   ▼
Radar + STM32 measure speed ──► ESP32 reads speed every loop
   │
   ▼
Speed ≥ Minimum Speed?  ──► start tracking the pass, follow the max speed
   │
   ▼
Max speed ≥ Photo Speed? ──► capture ONE photo
   │
   ▼
Vehicle leaves ──► upload max speed (+ photo if one was taken) to your account
```

Each pass uploads **exactly once**: a photo upload if the vehicle was speeding,
or a short speed-only record otherwise.

---

## What you need

- A MiniSpeedCam device, powered by **5 V** (USB-C, or the screw terminal). The
  easiest option is a USB-C power bank or wall adapter.
- A WiFi network (2.4 GHz) the device can join.
- A free account at **[minispeedcam.com](https://minispeedcam.com/)**.
- A phone or laptop for the one-time setup.

---

## First-time setup

### Step 1 — Create your account and camera

1. Sign up / log in at **[minispeedcam.com](https://minispeedcam.com/)**.
2. **Create a new camera** in your account.
3. **Copy the Camera ID** that minispeedcam.com gives you for that camera. You
   will paste this into the device in Step 4 — this is what links the physical
   device to the camera in your account, so its uploads show up under the right
   camera.

> **Why this matters:** every upload the device sends is tagged with the Camera
> ID. If the ID is missing or wrong, the server has no camera to attach the data
> to and rejects the upload. The device will not send anything until a valid
> Camera ID is saved.

### Step 2 — Power on and connect to the device's WiFi

The first time it powers on (with no WiFi saved), the device creates its own
WiFi access point:

- **Network name:** `MiniSpeedCam`
- **Password:** none (it's an open network)

On your phone or laptop, connect to the `MiniSpeedCam` network. (Some phones
list open networks lower down, or warn that it has "no internet" — that's
expected.)

### Step 3 — Open the configuration page

In a web browser, go to:

```
http://192.168.4.1
```

This opens the MiniSpeedCam configuration portal (three tabs: **Device**,
**Wifi Settings**, **Status**).

### Step 4 — Enter WiFi credentials and your Camera ID

Open the **Wifi Settings** tab and fill in:

| Field | What to enter |
|-------|---------------|
| **Network** | Your WiFi network name (SSID) |
| **Password** | Your WiFi password |
| **Camera ID:** | The Camera ID you copied from minispeedcam.com in Step 1 |

Then press **SAVE**. The device saves everything and **reboots**.

### Step 5 — Confirm it's online

After rebooting, the device joins your WiFi network instead of creating its own
AP. To check it's working:

- Reconnect your phone/laptop to your normal WiFi.
- Open the **Status** tab of the device's page (reach it at
  `http://MiniSpeedCam.local`, or use the IP shown in your router, or the "Open
  device UI" link in your minispeedcam.com dashboard).
- Confirm **IP** shows an address and **WiFi RSSI** shows a signal value.

The device is now linked to your camera. Drive past it (or trigger the radar)
above your Photo Speed and the photo + speed should appear in your
minispeedcam.com account.

---

## Settings reference

### Device tab

| Setting | What it does |
|---------|--------------|
| **Current Speed** | Live readout of the speed the radar is seeing right now (updates a few times per second). |
| **MPH / KPH** | Units for *all* speeds shown and stored. Off = MPH, On = KPH. Changing this changes the meaning of the Minimum Speed and Photo Speed numbers below. |
| **Power-Saver Mode** | On (default) = the device drops WiFi when idle to save power, reconnecting when a vehicle is detected. Off = the device stays awake with WiFi always up (handy while configuring or troubleshooting; uses more power). |
| **Minimum Speed** | The lowest speed (in your chosen units) that counts as a vehicle. Anything slower is ignored. Raise it to skip pedestrians/cyclists; lower it to catch slower traffic. |
| **Photo Speed** | The speed at which a photo is taken. A vehicle whose top speed reaches this value gets photographed; slower vehicles still record a speed but no photo. Set this to your "speeding" threshold. |

> **Minimum Speed vs. Photo Speed:** Minimum Speed decides what gets *tracked*;
> Photo Speed decides what gets *photographed*. A pass between the two values
> uploads a speed-only record. A pass at or above Photo Speed uploads a photo
> too. Keep Photo Speed ≥ Minimum Speed.

### Wifi Settings tab

| Control | What it does |
|---------|--------------|
| **Network** | Your WiFi SSID. |
| **Password** | Your WiFi password. |
| **Camera ID:** | The minispeedcam.com Camera ID that links this device to your camera. **Required for uploads to work.** |
| **SAVE** | Saves Network / Password / Camera ID to the device and reboots so the new settings take effect. |
| **CLEAR** | Wipes the saved WiFi credentials and reboots. The device then comes back up as the `MiniSpeedCam` access point so you can reconfigure it. |

### Status tab (read-only diagnostics)

| Label | Meaning |
|-------|---------|
| **Current Speed** | Live radar reading. |
| **Free heap / Free PSRAM** | Available memory — useful for spotting leaks/instability. |
| **Uptime** | Time since the last reboot. |
| **WiFi RSSI** | Signal strength in dBm (closer to 0 is stronger; e.g. −60 is good, −85 is weak). `n/a` when not connected. |
| **IP** | The device's address on your network. `n/a` when not connected. |
| **Last upload HTTP** | The HTTP status code of the most recent upload. `200`/`2xx` = success; `400` usually means the Camera ID isn't set or doesn't match an existing camera. |
| **Last reset reason** | Why the device last rebooted (power-on, crash, etc.). |
| **Boot count** | How many times the device has booted. |

---

## Re-entering setup later

If you move the device to a new network or need to change settings:

- **Clear Settings button** — on the Wifi Settings tab, press **CLEAR**. The
  device reboots into `MiniSpeedCam` AP mode for reconfiguration.
- **Physical WiFi-reset button** — press and hold the on-board WiFi-reset button
  for **3 seconds**. This erases the saved WiFi credentials and reboots into AP
  mode. (You must hold it continuously for the full 3 seconds; a quick tap does
  nothing.)

Either way, reconnect to the `MiniSpeedCam` network and repeat Steps 3–5.

---

## Troubleshooting

**The `MiniSpeedCam` WiFi network doesn't appear**
- It only appears when the device has **no valid WiFi saved**. If it previously
  connected to a network, it joins that network instead. Use **Clear Settings**
  or the 3-second WiFi-reset button to force it back into AP mode.
- It's an **open** network — check your "available networks" list carefully;
  some phones sort open networks lower or hide them under "Other networks."

**Uploads aren't appearing in my account / "Last upload HTTP" shows 400**
- The **Camera ID** is almost certainly missing or wrong. Open the Wifi Settings
  tab, paste the exact Camera ID from your minispeedcam.com camera, and press
  SAVE. The device skips uploading entirely until a Camera ID is set.

**Speeds look wrong or erratic / random readings with nothing passing**
- This is usually radio interference from WiFi coupling into the radar. The
  firmware filters out impossible readings and ignores one-off glitches, but if
  it persists, keeping the device on solid 5 V power (a quality supply/cable)
  and good antenna placement helps most.

**The device joins WiFi but the page won't load at `MiniSpeedCam.local`**
- mDNS (`.local`) isn't supported on every network/device. Use the **IP address**
  shown on the Status tab or in your router's device list instead.

---

## Power

MiniSpeedCam runs on **5 V**. It draws roughly **1.1 W** in full operation and
about **0.55 W** when idle (the radar sensor is always on). The simplest way to
run it is a USB-C power bank or wall adapter into either USB-C port. Battery
life depends on how much traffic triggers it.

---

## For developers

This guide is for setting up and using a device. For building/flashing the
firmware, the project layout, board options, and the capture/upload internals,
see the developer README: [README.md](README.md).
