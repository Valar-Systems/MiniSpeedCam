/**
 * variables.h - Shared global state for the MiniSpeedCam ESP32 firmware.
 *
 * Globals are grouped into small PODs by domain. Each struct is
 * instantiated exactly once in variables.cpp and consumed across both
 * cores. The Core 1 (radar polling) task writes into `radar`, raises
 * flags inside `upload`, and gates itself with `sleep_gate`; the Core 0
 * (networking) task drains those flags and consumes `net` for WiFi.
 */
#pragma once

#include <atomic>

#include <Arduino.h>
#include <Preferences.h>

// NVS-backed key/value store for WiFi creds and user settings.
extern Preferences preferences;

/** Live radar samples and user-configured thresholds. */
struct RadarState {
  int speed = 0;        // Latest sample from STM32 (units depend on is_kph)
  int maxSpeed = 0;     // Highest speed observed during current run
  int min_speed = 0;    // Begin tracking a vehicle at or above this speed
  int photo_speed = 0;  // Trigger camera capture at or above this speed
  bool is_kph = false;  // true = KPH, false = MPH (persisted in NVS)
};
extern RadarState radar;

/** Post-boot grace + measurement-blanking windows. */
struct SleepGate {
  bool wake_flag = false;          // true while the post-boot grace window is active
  bool ignore_flag = false;        // true: drop measurements during startup blanking
  unsigned long sleep_time = 0;    // Absolute millis() at which the grace window ends
  unsigned long ignore_time = 0;   // Absolute millis() at which the blanking ends
};
extern SleepGate sleep_gate;

/**
 * Core 1 -> Core 0 hand-off.
 *
 * Core 1 finishes a capture/measurement run, populates photo_filename
 * and publishes the framebuffer inside takePhoto(), then sets
 * speed_collection_complete (so the uploader knows maxSpeed is final)
 * and send_data (upload pending). connect_wifi is the orthogonal "WiFi
 * dropped, please reconnect" signal. Core 0 acts on the flags and
 * clears them.
 *
 * The three flags are std::atomic so that writes Core 1 performs before
 * raising a flag (populating photo_filename, publishing the camera
 * framebuffer inside takePhoto()) are guaranteed visible on Core 0 once
 * the flag observes the new value. Implicit conversions to/from bool
 * keep call sites identical to plain-bool code.
 */
struct UploadJob {
  std::atomic<bool> send_data{false};                  // Fresh capture ready to upload
  std::atomic<bool> speed_collection_complete{false};  // Core 1 has finalized maxSpeed
  std::atomic<bool> connect_wifi{false};               // Please (re)connect WiFi
  String photo_filename;                               // Filename field included in the upload payload
};
extern UploadJob upload;

// The pending camera framebuffer itself is owned inside camera.cpp and
// reached via cameraPendingPhoto*() accessors -- keeping esp_camera.h
// out of variables.h and out of every other module that consumes shared
// state. Ownership of the framebuffer transfers atomically between
// Core 1 (capture) and Core 0 (upload + release).

/** Persisted networking + cloud identity. */
struct NetConfig {
  String ssid;              // Stored WiFi SSID, "NOT_SET" until configured
  String password;          // Stored WiFi password, "NOT_SET" until configured
  String camera_id;         // minispeedcam.com camera identifier
  String local_ip_address;  // Most recent STA-mode IP (also sent to the cloud)
};
extern NetConfig net;

/** ESPUI control IDs for the credential text inputs (set in load_espui). */
struct EspuiHandles {
  uint16_t ssid_text = 0;
  uint16_t pass_text = 0;
  uint16_t camera_id_text = 0;
};
extern EspuiHandles ui_ids;
