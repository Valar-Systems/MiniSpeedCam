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
 * Toggle Power-Saver Mode.
 *
 * @param sender ESPUI Switcher control.
 * @param value  S_ACTIVE   = power-saver on  (radar loop drops WiFi when idle),
 *               S_INACTIVE = power-saver off (device never sleeps, WiFi stays up).
 */
void powerSaverCall(Control* sender, int value) {
  switch (value) {
    case S_ACTIVE:
      power_saver = true;
      break;
    case S_INACTIVE:
      power_saver = false;
      connect_wifi = true;  // ask Core 0 to bring WiFi back immediately (no-op if already up)
      break;
  }
  preferences.putBool("power_saver", power_saver);
  Serial.printf("[PWR] Power-Saver Mode %s\n", power_saver ? "ON" : "OFF");
}

/**
 * Toggle the aiming video stream (a temporary setup/mounting aid).
 *
 * S_ACTIVE requests a time-limited MJPEG stream (auto-stops after
 * STREAM_TIMEOUT_MS); S_INACTIVE stops it. Only sets the desired-state flags
 * here — taskCore1's streamService() performs the actual camera/server work
 * and updates the URL label, so all of that stays on one task.
 */
void aimingStreamCall(Control* sender, int value) {
  switch (value) {
    case S_ACTIVE:
      stream_deadline = millis() + STREAM_TIMEOUT_MS;
      stream_active = true;
      break;
    case S_INACTIVE:
      stream_active = false;
      break;
  }
  Serial.printf("[STREAM] requested %s\n", stream_active ? "ON" : "OFF");
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

/**
 * Persist the minimum radar echo strength (proximity gate) required to arm a
 * run. The STM32 reports an FFT peak magnitude alongside each speed; magnitude
 * tracks received power (~1/r^4), so distant cross-traffic reads weak. Set this
 * between the "Signal" readings you see for a car on the road vs. a distant one
 * (watch the Status tab). 0 = disabled.
 */
void minSignalCall(Control* sender, int type) {
  Serial.print(", Value: ");
  Serial.println(sender->value);
  min_signal = sender->value.toInt();
  preferences.putInt("min_signal", min_signal);
}

/**
 * Persist the echo magnitude (proximity) at which to FIRE THE PHOTO. With the
 * unit aimed down the road, an oncoming car's magnitude rises and a receding
 * car's falls, so the firmware shoots when the live magnitude crosses this
 * value -- catching the front plate on the way in and the rear plate on the way
 * out. Set it (watching the Status "Signal" reading) to the value a car shows
 * when it's best framed for the plate; keep it above "Min Signal". 0 = off
 * (legacy: capture at Photo Speed).
 */
void photoSignalCall(Control* sender, int type) {
  Serial.print(", Value: ");
  Serial.println(sender->value);
  photo_signal = sender->value.toInt();
  preferences.putInt("photo_signal", photo_signal);
}

/**
 * Persist the FRONT (oncoming) fire threshold. A car's front reflects strongly,
 * so an oncoming car reaches a given magnitude while still far -- RAISE this to
 * delay the front-plate shot until the car is closer/better framed. 0 = inherit
 * the shared "Photo Signal" value. Keep it >= "Min Signal".
 */
void photoSignalFrontCall(Control* sender, int type) {
  photo_signal_front = sender->value.toInt();
  preferences.putInt("ps_front", photo_signal_front);
}

/**
 * Persist the REAR (receding) fire threshold. A car's rear reflects weakly, and
 * the shot wants the car a bit farther out (not broadside) -- LOWER this to fire
 * the rear-plate shot later, once the receding car has pulled away. 0 = inherit
 * the shared "Photo Signal" value.
 */
void photoSignalRearCall(Control* sender, int type) {
  photo_signal_rear = sender->value.toInt();
  preferences.putInt("ps_rear", photo_signal_rear);
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
    String ssid = ESPUI.getControl(wifi_ssid_text)->value;
    String pass = ESPUI.getControl(wifi_pass_text)->value;
    String camera_id = ESPUI.getControl(camera_id_text)->value;
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
void buttonClearNetworkCall(Control* sender, int type) {
  if (type == B_UP) {
    preferences.putInt("wifi_set", 0);
    preferences.putString("ssid", "NOT_SET");
    preferences.putString("pass", "NOT_SET");
    preferences.end();
    ESP.restart();
  }
}

// ----- Status tab live-update handles -----
// Label controls created in load_espui() and refreshed by
// updateStatusDisplay(). Like updateSpeedDisplay(), that runs on Core 1 so
// all ESPUI WebSocket writes come from a single task.
static uint16_t status_speed_label = 0;
static uint16_t status_signal_label = 0;  // live radar echo magnitude (proximity calibration)
static uint16_t status_heap_label = 0;
static uint16_t status_psram_label = 0;
static uint16_t status_uptime_label = 0;
static uint16_t status_rssi_label = 0;
static uint16_t status_ip_label = 0;
static uint16_t status_last_upload_label = 0;
static uint16_t status_reset_reason_label = 0;
static uint16_t status_boot_count_label = 0;

/**
 * Build the ESPUI control tree and start serving it.
 *
 * Tab 1 ("Device"):
 *   - Live speed readout (updated by taskCore1 via updateSpeedDisplay())
 *   - MPH/KPH switcher
 *   - Power-Saver Mode switcher (drop WiFi when idle vs. never sleep)
 *   - Minimum speed (run threshold)
 *   - Photo speed (capture threshold)
 *   - Aiming Stream switcher + URL label (temporary MJPEG video for mounting)
 *
 * Tab 2 ("Wifi Settings"):
 *   - Clear Settings button
 *   - Network / Password / Camera ID text inputs
 *   - Save Settings button
 *
 * Tab 3 ("Status"):
 *   - Live diagnostics (speed, free heap/PSRAM, uptime, RSSI, IP, last
 *     upload HTTP code, reset reason, boot count). Refreshed ~1 Hz from
 *     updateStatusDisplay().
 */
void load_espui(void) {
  uint16_t tab1 = ESPUI.addControl(ControlType::Tab, "Device", "Device");
  uint16_t tab2 = ESPUI.addControl(ControlType::Tab, "Wifi Settings", "Wifi Settings");
  uint16_t tab3 = ESPUI.addControl(ControlType::Tab, "Status", "Status");

  //tab1: Device settings
  labelSpeed = ESPUI.addControl(ControlType::Label, "Current Speed", "—", ControlColor::Turquoise, tab1);
  ESPUI.addControl(ControlType::Switcher, "MPH/KPH:", String(is_kph), ControlColor::Alizarin, tab1, &speedUnitsCall);
  ESPUI.addControl(ControlType::Switcher, "Power-Saver Mode (drop WiFi when idle):", String(power_saver), ControlColor::Alizarin, tab1, &powerSaverCall);
  ESPUI.addControl(ControlType::Number, "Minimum Speed:", String(min_speed), ControlColor::Alizarin, tab1, &minSpeedCall);
  ESPUI.addControl(ControlType::Number, "Photo Speed:", String(photo_speed), ControlColor::Alizarin, tab1, &photoSpeedCall);
  ESPUI.addControl(ControlType::Number, "Min Signal (proximity, 0=off):", String(min_signal), ControlColor::Alizarin, tab1, &minSignalCall);
  ESPUI.addControl(ControlType::Number, "Photo Signal (shared/default, 0=off):", String(photo_signal), ControlColor::Alizarin, tab1, &photoSignalCall);
  ESPUI.addControl(ControlType::Number, "Photo Signal FRONT (oncoming; raise=closer, 0=use shared):", String(photo_signal_front), ControlColor::Alizarin, tab1, &photoSignalFrontCall);
  ESPUI.addControl(ControlType::Number, "Photo Signal REAR (receding; lower=farther, 0=use shared):", String(photo_signal_rear), ControlColor::Alizarin, tab1, &photoSignalRearCall);

  //tab1: Aiming video stream (temporary setup aid; auto-stops after 5 min)
  aimingSwitchId = ESPUI.addControl(ControlType::Switcher, "Aiming Stream (video, 5 min):", "0", ControlColor::Sunflower, tab1, &aimingStreamCall);
  labelStream = ESPUI.addControl(ControlType::Label, "Stream URL", "off", ControlColor::Sunflower, tab1);

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

  //tab3: Status (live diagnostics, refreshed by updateStatusDisplay())
  status_speed_label        = ESPUI.addControl(ControlType::Label, "Current Speed",     "—",          ControlColor::Peterriver, tab3);
  status_signal_label       = ESPUI.addControl(ControlType::Label, "Signal (proximity)", "0",         ControlColor::Peterriver, tab3);
  status_heap_label         = ESPUI.addControl(ControlType::Label, "Free heap (B)",     "0",          ControlColor::Peterriver, tab3);
  status_psram_label        = ESPUI.addControl(ControlType::Label, "Free PSRAM (B)",    "0",          ControlColor::Peterriver, tab3);
  status_uptime_label       = ESPUI.addControl(ControlType::Label, "Uptime",            "0h 00m 00s", ControlColor::Peterriver, tab3);
  status_rssi_label         = ESPUI.addControl(ControlType::Label, "WiFi RSSI (dBm)",   "n/a",        ControlColor::Peterriver, tab3);
  status_ip_label           = ESPUI.addControl(ControlType::Label, "IP",                "n/a",        ControlColor::Peterriver, tab3);
  status_last_upload_label  = ESPUI.addControl(ControlType::Label, "Last upload HTTP",  "n/a",        ControlColor::Peterriver, tab3);
  status_reset_reason_label = ESPUI.addControl(ControlType::Label, "Last reset reason", diagnosticsResetReason(),        ControlColor::Peterriver, tab3);
  status_boot_count_label   = ESPUI.addControl(ControlType::Label, "Boot count",        String(diagnosticsBootCount()),  ControlColor::Peterriver, tab3);

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

/**
 * Push the live speed reading to the ESPUI "Current Speed" label.
 *
 * Called from taskCore1 (Core 1) on every radar poll. Throttled to ~3 Hz and
 * only sends when the value changes, so the WebSocket isn't flooded while the
 * speed sits at a steady value. Safe to call before/while no client is
 * connected — ESPUI simply has no one to push to.
 */
void updateSpeedDisplay(void) {
  static unsigned long lastUpdate = 0;
  static int lastShown = 0;
  static bool initialized = false;

  if (initialized && speed == lastShown) return;          // nothing changed; skip
  unsigned long now = millis();
  if (initialized && (now - lastUpdate) < 300) return;    // throttle to ~3 Hz

  lastUpdate = now;
  lastShown = speed;
  initialized = true;
  ESPUI.updateLabel(labelSpeed, String(speed) + (is_kph ? " KPH" : " MPH"));
}

/**
 * Refresh the Status-tab labels (heap, PSRAM, uptime, RSSI, IP, etc.).
 *
 * Called from taskCore1 (the only task that touches ESPUI) and throttled to
 * ~1 Hz, since each updateLabel broadcasts to every connected client. Reads
 * cross-task data (speed, last-upload code) that is written elsewhere as
 * plain scalars — benign for a display refresh.
 */
void updateStatusDisplay(void) {
  static unsigned long last_ms = 0;
  if (millis() - last_ms < 1000) return;
  last_ms = millis();

  char uptime_buf[32];
  diagnosticsFormatUptime(uptime_buf, sizeof(uptime_buf));

  bool connected = (WiFi.status() == WL_CONNECTED);
  ESPUI.updateLabel(status_speed_label, String(speed) + (is_kph ? " KPH" : " MPH"));
  ESPUI.updateLabel(status_signal_label, String(g_last_peak_mag));  // live echo strength for proximity calibration
  ESPUI.updateLabel(status_heap_label, String(ESP.getFreeHeap()));
  ESPUI.updateLabel(status_psram_label, String(ESP.getFreePsram()));
  ESPUI.updateLabel(status_uptime_label, String(uptime_buf));
  ESPUI.updateLabel(status_rssi_label, connected ? String(WiFi.RSSI()) : String("n/a"));
  ESPUI.updateLabel(status_ip_label, connected ? WiFi.localIP().toString() : String("n/a"));
  ESPUI.updateLabel(status_last_upload_label, String(diagnosticsLastUploadCode()));
}