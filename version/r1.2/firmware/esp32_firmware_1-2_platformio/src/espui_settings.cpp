#include "espui_settings.h"

#include <Arduino.h>
#include <ESPUI.h>

#include "variables.h"

/**
 * Toggle MPH (off) vs KPH (on).
 *
 * @param sender ESPUI Switcher control.
 * @param value  S_ACTIVE = KPH, S_INACTIVE = MPH.
 */
static void speedUnitsCall(Control* sender, int value) {
  Serial.print(", Value: ");
  Serial.println(sender->value);

  switch (value) {
    case S_ACTIVE:
      Serial.print("Active:");
      radar.is_kph = true;
      break;

    case S_INACTIVE:
      Serial.print("Inactive");
      radar.is_kph = false;
      break;
  }

  Serial.print(" ");
  Serial.println(sender->id);
  preferences.putBool("is_kph", radar.is_kph);
}

/**
 * Persist the minimum speed at which a tracking run begins.
 * Below this value, samples are ignored entirely.
 */
// User-facing speed thresholds. Anything outside this band is either a
// typo or a hostile payload; clamp before persisting.
static constexpr int kSpeedMin = 1;
static constexpr int kSpeedMax = 200;

static void minSpeedCall(Control* sender, int type) {
  Serial.print(", Value: ");
  Serial.println(sender->value);
  radar.min_speed = constrain(sender->value.toInt(), kSpeedMin, kSpeedMax);
  preferences.putInt("min_speed", radar.min_speed);
}

/**
 * Persist the speed at which a photo is captured during a run.
 * Runs whose max speed never reaches photo_speed upload speed-only.
 */
static void photoSpeedCall(Control* sender, int type) {
  Serial.print(", Value: ");
  Serial.println(sender->value);
  radar.photo_speed = constrain(sender->value.toInt(), kSpeedMin, kSpeedMax);
  preferences.putInt("photo_speed", radar.photo_speed);
}

// Camera ID is read directly off the ESPUI control inside
// buttonSaveNetworkCall(), so per-keystroke handling is unnecessary.
static void textCameraIdCall(Control* sender, int type) {
  // Leave blank
}

// SSID is read on Save (see buttonSaveNetworkCall); no live update needed.
static void textNetworkCall(Control* sender, int type) {
  //    ssid = sender->value;
  //    Serial.print(ssid);
}

// Password is read on Save (see buttonSaveNetworkCall); no live update needed.
static void textPasswordCall(Control* sender, int type) {
  //    Serial.print(sender->value);
  //    pass = sender->value;
  //    Serial.print(pass);
}

/**
 * Save Settings button: snapshots SSID/password/camera_id from the
 * ESPUI text inputs into NVS, then reboots so the new credentials take
 * effect via connectWifiAP() at startup.
 */
static void buttonSaveNetworkCall(Control* sender, int type) {
  if (type == B_UP) {
    Serial.println("Button Pressed");
    String ssid      = ESPUI.getControl(ui_ids.ssid_text)->value;
    String pass      = ESPUI.getControl(ui_ids.pass_text)->value;
    String camera_id = ESPUI.getControl(ui_ids.camera_id_text)->value;
    preferences.putString("ssid", ssid);
    preferences.putString("pass", pass);
    preferences.putString("camera_id", camera_id);
    ESP.restart();
  }
}

/**
 * Clear Settings button: wipes stored WiFi credentials and reboots.
 *
 * After reboot, connectWifiAP() will fail to associate and bring up
 * the captive AP for reconfiguration.
 */
static void buttonClearNetworkCall(Control* sender, int type) {
  if (type == B_UP) {
    preferences.putInt("wifi_set", 0);
    preferences.putString("ssid", "NOT_SET");
    preferences.putString("pass", "NOT_SET");
    preferences.end();
    ESP.restart();
  }
}

/**
 * Build the ESPUI control tree and start serving it.
 *
 * Tab 1 ("Device"):
 *   - MPH/KPH switcher
 *   - Minimum speed (run threshold)
 *   - Photo speed (capture threshold)
 *
 * Tab 2 ("Wifi Settings"):
 *   - Clear Settings button
 *   - Network / Password / Camera ID text inputs
 *   - Save Settings button
 */
void load_espui() {
  uint16_t tab1 = ESPUI.addControl(ControlType::Tab, "Device", "Device");
  uint16_t tab2 = ESPUI.addControl(ControlType::Tab, "Wifi Settings", "Wifi Settings");

  //tab1: Device settings
  ESPUI.addControl(ControlType::Switcher, "MPH/KPH:",       String(radar.is_kph),      ControlColor::Alizarin, tab1, &speedUnitsCall);
  ESPUI.addControl(ControlType::Number,   "Minimum Speed:", String(radar.min_speed),   ControlColor::Alizarin, tab1, &minSpeedCall);
  ESPUI.addControl(ControlType::Number,   "Photo Speed:",   String(radar.photo_speed), ControlColor::Alizarin, tab1, &photoSpeedCall);

  //tab2: WiFi
  ESPUI.addControl(ControlType::Separator, "Wifi Status", "", ControlColor::None, tab2);

  //Button: Clear Network Settings
  ESPUI.addControl(ControlType::Button, "Clear Settings", "CLEAR", ControlColor::Emerald, tab2, &buttonClearNetworkCall);

  //Button: Network Settings
  ESPUI.addControl(ControlType::Separator, "Set Wifi", "", ControlColor::None, tab2);
  ui_ids.ssid_text      = ESPUI.addControl(ControlType::Text, "Network",    String(net.ssid),      ControlColor::Emerald,    tab2, &textNetworkCall);     //Text: Network
  ui_ids.pass_text      = ESPUI.addControl(ControlType::Text, "Password",   String(net.password),  ControlColor::Emerald,    tab2, &textPasswordCall);    //Text: Password
  ui_ids.camera_id_text = ESPUI.addControl(ControlType::Text, "Camera ID:", String(net.camera_id), ControlColor::Peterriver, tab2, &textCameraIdCall);    //Text: Camera ID

  //Button: Save
  ESPUI.addControl(ControlType::Button, "Save Settings", "SAVE", ControlColor::Emerald, tab2, &buttonSaveNetworkCall);

  /*
     * .begin loads and serves all files from PROGMEM directly.
     * If you want to serve the files from LITTLEFS use ESPUI.beginLITTLEFS
     * (.prepareFileSystem has to be run in an empty sketch before)
     */

  // Enable this option if you want sliders to be continuous (update during move) and not discrete (update on stop)
  // ESPUI.sliderContinuous = true;

  /*
     * Optionally you can use HTTP BasicAuth. Keep in mind that this is NOT a
     * SECURE way of limiting access.
     * Anyone who is able to sniff traffic will be able to intercept your password
     * since it is transmitted in cleartext. Just add a string as username and
     * password, for example begin("ESPUI Control", "username", "password")
     */

  ESPUI.begin("ESPUI Control");
}
