/**
 * espui_settings.h - ESPUI web UI controls and persistence callbacks.
 *
 * Builds the configuration portal exposed at the device's IP:
 *   - "Device" tab:  speed units toggle, minimum speed, photo speed.
 *   - "Wifi Settings" tab: SSID/password/camera-id text inputs, plus
 *     Save (writes to NVS and reboots) and Clear Settings buttons.
 *
 * Each control's callback updates the matching global in variables.h
 * and persists it via the shared `preferences` (NVS) instance.
 */
#pragma once

/**
 * Build the ESPUI control tree and start serving it.
 */
void load_espui();
