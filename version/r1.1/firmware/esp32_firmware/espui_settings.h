// Set KPH/MPH
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

// Minimum speed to trigger any radar
void minSpeedCall(Control* sender, int type) {
  Serial.print(", Value: ");
  Serial.println(sender->value);
  min_speed = sender->value.toInt();
  preferences.putInt("min_speed", min_speed);
}

void photoSpeedCall(Control* sender, int type) {
  Serial.print(", Value: ");
  Serial.println(sender->value);
  photo_speed = sender->value.toInt();
  preferences.putInt("photo_speed", photo_speed);
}

void textCameraIdCall(Control* sender, int type) {
// Leave blank
}

void textNetworkCall(Control* sender, int type) {
  //    ssid = sender->value;
  //    Serial.print(ssid);
}

void textPasswordCall(Control* sender, int type) {
  //    Serial.print(sender->value);
  //    pass = sender->value;
  //    Serial.print(pass);
}

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

void buttonClearNetworkCall(Control* sender, int type) {
  if (type == B_UP) {
    preferences.putInt("wifi_set", 0);
    preferences.putString("ssid", "NOT_SET");
    preferences.putString("pass", "NOT_SET");
    preferences.end();
    ESP.restart();
  }
}

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