#include "espui_settings.h"

#include <Arduino.h>
#include <WiFi.h>
#include <ESPUI.h>

#include "camera.h"
#include "diagnostics.h"
#include "ota.h"
#include "queue.h"
#include "variables.h"

// ----- Callback bodies for the Device tab -----

/**
 * Toggle MPH (off) vs KPH (on).
 */
static void speedUnitsCall(Control* sender, int value) {
  radar.is_kph = (value == S_ACTIVE);
  preferences.putBool("is_kph", radar.is_kph);
}

// User-facing speed thresholds. Anything outside this band is either a
// typo or a hostile payload; clamp before persisting.
static constexpr int kSpeedMin = 1;
static constexpr int kSpeedMax = 200;

static void minSpeedCall(Control* sender, int /*type*/) {
  radar.min_speed = constrain(sender->value.toInt(), kSpeedMin, kSpeedMax);
  preferences.putInt("min_speed", radar.min_speed);
}

static void photoSpeedCall(Control* sender, int /*type*/) {
  radar.photo_speed = constrain(sender->value.toInt(), kSpeedMin, kSpeedMax);
  preferences.putInt("photo_speed", radar.photo_speed);
}

/**
 * Service mode switch: when on, sleep paths are skipped and WiFi stays
 * up so OTA pushes and the web UI remain reachable indefinitely.
 */
static void serviceModeCall(Control* /*sender*/, int value) {
  bool enabled = (value == S_ACTIVE);
  service_mode.store(enabled);
  preferences.putBool("service_mode", enabled);
  Serial.printf("Service mode %s\n", enabled ? "ON" : "OFF");
}

/** JPEG quality 0..63 (lower = better quality). */
static void jpegQualityCall(Control* sender, int /*type*/) {
  cameraSetJpegQuality(sender->value.toInt());
}

/** Framesize select (FRAMESIZE_* enum value carried as the option's value). */
static void frameSizeCall(Control* sender, int /*type*/) {
  cameraSetFrameSize(sender->value.toInt());
}

// ----- Callback bodies for the WiFi Settings tab -----

// Camera ID, SSID, password are all read on Save; nothing to do per-keystroke.
static void textCameraIdCall(Control* /*sender*/, int /*type*/) {}
static void textNetworkCall(Control* /*sender*/, int /*type*/) {}
static void textPasswordCall(Control* /*sender*/, int /*type*/) {}

/**
 * Save Settings button: snapshots SSID / password / camera_id / OTA
 * password into NVS, then reboots so the new credentials take effect
 * via connectWifiAP() at startup.
 */
static uint16_t ota_password_text = 0;
static void buttonSaveNetworkCall(Control* /*sender*/, int type) {
  if (type == B_UP) {
    Serial.println("Save Settings pressed");
    String ssid      = ESPUI.getControl(ui_ids.ssid_text)->value;
    String pass      = ESPUI.getControl(ui_ids.pass_text)->value;
    String camera_id = ESPUI.getControl(ui_ids.camera_id_text)->value;
    String ota_pass  = ESPUI.getControl(ota_password_text)->value;
    preferences.putString("ssid", ssid);
    preferences.putString("pass", pass);
    preferences.putString("camera_id", camera_id);
    if (ota_pass.length() > 0) {
      otaSetPassword(ota_pass);
    }
    ESP.restart();
  }
}

/**
 * Clear Settings button: wipes stored WiFi credentials and reboots.
 */
static void buttonClearNetworkCall(Control* /*sender*/, int type) {
  if (type == B_UP) {
    preferences.putInt("wifi_set", 0);
    preferences.putString("ssid", "NOT_SET");
    preferences.putString("pass", "NOT_SET");
    preferences.end();
    ESP.restart();
  }
}

// ----- Status tab live-update handles -----
//
// These point at Label controls created in load_espui(). The values are
// pushed from espuiDiagnosticsTick() (called from taskCore0) so the
// page reflects current speed, free heap, uptime, etc., without
// refreshing.

static uint16_t status_speed_label = 0;
static uint16_t status_heap_label = 0;
static uint16_t status_psram_label = 0;
static uint16_t status_uptime_label = 0;
static uint16_t status_rssi_label = 0;
static uint16_t status_ip_label = 0;
static uint16_t status_last_upload_label = 0;
static uint16_t status_queue_label = 0;
static uint16_t status_reset_reason_label = 0;
static uint16_t status_boot_count_label = 0;

/**
 * Build the ESPUI control tree and start serving it.
 *
 * Tab 1 ("Device"):
 *   - MPH/KPH switcher, min speed, photo speed
 *   - Service mode toggle (keep awake for OTA / debugging)
 *   - JPEG quality, framesize
 *
 * Tab 2 ("Wifi Settings"):
 *   - Clear Settings button
 *   - Network / Password / Camera ID / OTA password text inputs
 *   - Save Settings button
 *
 * Tab 3 ("Status"):
 *   - Live diagnostics (speed, heap, uptime, RSSI, IP, last upload,
 *     pending queue, reset reason, boot count). Refreshed from
 *     espuiDiagnosticsTick().
 */
void load_espui() {
  uint16_t tab1 = ESPUI.addControl(ControlType::Tab, "Device",        "Device");
  uint16_t tab2 = ESPUI.addControl(ControlType::Tab, "Wifi Settings", "Wifi Settings");
  uint16_t tab3 = ESPUI.addControl(ControlType::Tab, "Status",        "Status");

  // ----- tab1: Device settings -----
  ESPUI.addControl(ControlType::Switcher, "MPH/KPH:",            String(radar.is_kph),         ControlColor::Alizarin, tab1, &speedUnitsCall);
  ESPUI.addControl(ControlType::Number,   "Minimum Speed:",      String(radar.min_speed),      ControlColor::Alizarin, tab1, &minSpeedCall);
  ESPUI.addControl(ControlType::Number,   "Photo Speed:",        String(radar.photo_speed),    ControlColor::Alizarin, tab1, &photoSpeedCall);
  ESPUI.addControl(ControlType::Switcher, "Service Mode (no sleep, OTA-reachable):",
                                          String(service_mode.load() ? 1 : 0),
                                          ControlColor::Sunflower, tab1, &serviceModeCall);

  ESPUI.addControl(ControlType::Separator, "Image", "", ControlColor::None, tab1);
  ESPUI.addControl(ControlType::Number,   "JPEG Quality (0-63, lower=better):",
                                          String(cameraGetJpegQuality()),
                                          ControlColor::Wetasphalt, tab1, &jpegQualityCall);

  uint16_t fs_select = ESPUI.addControl(ControlType::Select, "Frame size:",
                                        String(cameraGetFrameSize()),
                                        ControlColor::Wetasphalt, tab1, &frameSizeCall);
  // FRAMESIZE_* enum values, as documented in esp_camera/sensor.h.
  ESPUI.addControl(ControlType::Option, "QVGA  (320x240)",   "5",  ControlColor::Wetasphalt, fs_select);
  ESPUI.addControl(ControlType::Option, "VGA   (640x480)",   "8",  ControlColor::Wetasphalt, fs_select);
  ESPUI.addControl(ControlType::Option, "SVGA  (800x600)",   "9",  ControlColor::Wetasphalt, fs_select);
  ESPUI.addControl(ControlType::Option, "XGA   (1024x768)",  "10", ControlColor::Wetasphalt, fs_select);
  ESPUI.addControl(ControlType::Option, "SXGA  (1280x1024)", "12", ControlColor::Wetasphalt, fs_select);
  ESPUI.addControl(ControlType::Option, "UXGA  (1600x1200)", "13", ControlColor::Wetasphalt, fs_select);

  // ----- tab2: WiFi settings -----
  ESPUI.addControl(ControlType::Separator, "Wifi Status", "", ControlColor::None, tab2);
  ESPUI.addControl(ControlType::Button, "Clear Settings", "CLEAR", ControlColor::Emerald, tab2, &buttonClearNetworkCall);

  ESPUI.addControl(ControlType::Separator, "Set Wifi", "", ControlColor::None, tab2);
  ui_ids.ssid_text      = ESPUI.addControl(ControlType::Text, "Network",      String(net.ssid),      ControlColor::Emerald,    tab2, &textNetworkCall);
  ui_ids.pass_text      = ESPUI.addControl(ControlType::Text, "Password",     String(net.password),  ControlColor::Emerald,    tab2, &textPasswordCall);
  ui_ids.camera_id_text = ESPUI.addControl(ControlType::Text, "Camera ID:",   String(net.camera_id), ControlColor::Peterriver, tab2, &textCameraIdCall);
  ota_password_text     = ESPUI.addControl(ControlType::Text, "OTA Password (blank to keep current):", String(""), ControlColor::Peterriver, tab2);

  ESPUI.addControl(ControlType::Button, "Save Settings", "SAVE", ControlColor::Emerald, tab2, &buttonSaveNetworkCall);

  // ----- tab3: Status -----
  status_speed_label        = ESPUI.addControl(ControlType::Label, "Current speed",     "0",       ControlColor::Peterriver, tab3);
  status_heap_label         = ESPUI.addControl(ControlType::Label, "Free heap (B)",     "0",       ControlColor::Peterriver, tab3);
  status_psram_label        = ESPUI.addControl(ControlType::Label, "Free PSRAM (B)",    "0",       ControlColor::Peterriver, tab3);
  status_uptime_label       = ESPUI.addControl(ControlType::Label, "Uptime",            "0h 00m 00s", ControlColor::Peterriver, tab3);
  status_rssi_label         = ESPUI.addControl(ControlType::Label, "WiFi RSSI (dBm)",   "n/a",     ControlColor::Peterriver, tab3);
  status_ip_label           = ESPUI.addControl(ControlType::Label, "IP",                "n/a",     ControlColor::Peterriver, tab3);
  status_last_upload_label  = ESPUI.addControl(ControlType::Label, "Last upload HTTP",  "n/a",     ControlColor::Peterriver, tab3);
  status_queue_label        = ESPUI.addControl(ControlType::Label, "Pending queue",     "0",       ControlColor::Peterriver, tab3);
  status_reset_reason_label = ESPUI.addControl(ControlType::Label, "Last reset reason", diagnosticsResetReason(), ControlColor::Peterriver, tab3);
  status_boot_count_label   = ESPUI.addControl(ControlType::Label, "Boot count",        String(diagnosticsBootCount()), ControlColor::Peterriver, tab3);

  ESPUI.begin("ESPUI Control");
}

void espuiDiagnosticsTick() {
  // Throttle to ~once a second; ESPUI broadcasts each updateLabel to
  // every connected client, so flooding hurts both the device and the
  // page.
  static unsigned long last_ms = 0;
  if (millis() - last_ms < 1000) return;
  last_ms = millis();

  char uptime_buf[32];
  diagnosticsFormatUptime(uptime_buf, sizeof(uptime_buf));

  ESPUI.updateLabel(status_speed_label,        String(radar.speed));
  ESPUI.updateLabel(status_heap_label,         String(ESP.getFreeHeap()));
  ESPUI.updateLabel(status_psram_label,        String(ESP.getFreePsram()));
  ESPUI.updateLabel(status_uptime_label,       String(uptime_buf));
  ESPUI.updateLabel(status_rssi_label,
                    WiFi.status() == WL_CONNECTED ? String(WiFi.RSSI()) : String("n/a"));
  ESPUI.updateLabel(status_ip_label,
                    WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("n/a"));
  ESPUI.updateLabel(status_last_upload_label,  String(diagnosticsLastUploadCode()));
  ESPUI.updateLabel(status_queue_label,        String((unsigned)queuePendingCount()));
}
