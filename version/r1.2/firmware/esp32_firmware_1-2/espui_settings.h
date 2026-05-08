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

/**
 * Toggle MPH (off) vs KPH (on).
 *
 * @param sender ESPUI Switcher control.
 * @param value  S_ACTIVE = KPH, S_INACTIVE = MPH.
 */
void speedUnitsCall(Control* sender, int value) {
  Serial.print(", Value: ");
  Serial.println(sender->value);

  switch (value) {
    case S_ACTIVE:
      Serial.print("Active:");
      is_kph = true;
      break;

    case S_INACTIVE:
      Serial.print("Inactive");
      is_kph = false;
      break;
  }

  Serial.print(" ");
  Serial.println(sender->id);
  preferences.putBool("is_kph", is_kph);
}

/**
 * Persist the minimum speed at which a tracking run begins.
 * Below this value, samples are ignored entirely.
 */
void minSpeedCall(Control* sender, int type) {
  Serial.print(", Value: ");
  Serial.println(sender->value);
  min_speed = sender->value.toInt();
  preferences.putInt("min_speed", min_speed);
}

/**
 * Persist the speed at which a photo is captured during a run.
 * Runs whose max speed never reaches photo_speed upload speed-only.
 */
void photoSpeedCall(Control* sender, int type) {
  Serial.print(", Value: ");
  Serial.println(sender->value);
  photo_speed = sender->value.toInt();
  preferences.putInt("photo_speed", photo_speed);
}

// Camera ID is read directly off the ESPUI control inside
// buttonSaveNetworkCall(), so per-keystroke handling is unnecessary.
void textCameraIdCall(Control* sender, int type) {
// Leave blank
}

// SSID is read on Save (see buttonSaveNetworkCall); no live update needed.
void textNetworkCall(Control* sender, int type) {
  //    ssid = sender->value;
  //    Serial.print(ssid);
}

// Password is read on Save (see buttonSaveNetworkCall); no live update needed.
void textPasswordCall(Control* sender, int type) {
  //    Serial.print(sender->value);
  //    pass = sender->value;
  //    Serial.print(pass);
}

/**
 * Save Settings button: snapshots SSID/password/camera_id from the
 * ESPUI text inputs into NVS, then reboots so the new credentials take
 * effect via connectWifiAP() at startup.
 */
void buttonSaveNetworkCall(Control* sender, int type) {
  if (type == B_UP) {
    Serial.println("Button Pressed");
    // Renamed to avoid shadowing the globals declared in variables.h.
    String new_ssid = ESPUI.getControl(wifi_ssid_text)->value;
    String new_pass = ESPUI.getControl(wifi_pass_text)->value;
    String new_camera_id = ESPUI.getControl(camera_id_text)->value;
    preferences.putString("ssid", new_ssid);
    preferences.putString("pass", new_pass);
    preferences.putString("camera_id", new_camera_id);
    ESP.restart();
  }
}

/**
 * Clear Settings button: wipes stored WiFi credentials and reboots.
 *
 * After reboot, connectWifiAP() will fail to associate and bring up
 * the captive AP for reconfiguration.
 */
void buttonClearNetworkCall(Control* sender, int type) {
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
void load_espui(void) {
  uint16_t tab1 = ESPUI.addControl(ControlType::Tab, "Device", "Device");
  uint16_t tab2 = ESPUI.addControl(ControlType::Tab, "Wifi Settings", "Wifi Settings");

  //tab1: Device settings
  ESPUI.addControl(ControlType::Switcher, "MPH/KPH:", String(is_kph), ControlColor::Alizarin, tab1, &speedUnitsCall);
  ESPUI.addControl(ControlType::Number, "Minimum Speed:", String(min_speed), ControlColor::Alizarin, tab1, &minSpeedCall);
  ESPUI.addControl(ControlType::Number, "Photo Speed:", String(photo_speed), ControlColor::Alizarin, tab1, &photoSpeedCall);

  //tab2: WiFi
  ESPUI.addControl(ControlType::Separator, "Wifi Status", "", ControlColor::None, tab2);

  //Button: Clear Network Settings
  ESPUI.addControl(ControlType::Button, "Clear Settings", "CLEAR", ControlColor::Emerald, tab2, &buttonClearNetworkCall);

  //Button: Network Settings
  ESPUI.addControl(ControlType::Separator, "Set Wifi", "", ControlColor::None, tab2);
  wifi_ssid_text = ESPUI.addControl(ControlType::Text, "Network", String(ssid), ControlColor::Emerald, tab2, &textNetworkCall); //Text: Network
  wifi_pass_text = ESPUI.addControl(ControlType::Text, "Password", String(password), ControlColor::Emerald, tab2, &textPasswordCall); //Text: Password
  camera_id_text = ESPUI.addControl(ControlType::Text, "Camera ID:", String(camera_id), ControlColor::Peterriver, tab2, &textCameraIdCall); //Text: Camera ID

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