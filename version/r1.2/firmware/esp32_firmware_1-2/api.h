/**
 * api.h - WiFi management and HTTPS uploads to minispeedcam.com.
 *
 * Provides:
 *   - connectWifiAP():   first-boot bring-up; tries STA, otherwise opens
 *                        a "MiniSpeedCam" soft-AP for the captive ESPUI.
 *   - connectWifi():     reconnect helper used by Core 0 when the radar
 *                        wakes the device and STA is dropped.
 *   - disconnectWifi():  cleanly tear down WiFi (used during sleep paths).
 *   - wifiResetButton(): 3-second long-press on WIFI_RESET_PIN clears
 *                        stored credentials and reboots into AP mode.
 *   - sendLocalIP():     announce the device's LAN IP to the cloud so
 *                        the user can reach the ESPUI portal remotely.
 *   - sendPhoto():       POST the captured photo + max speed (or just
 *                        the speed when below the photo threshold).
 *
 * NOTE: the Bearer token below is a public Bubble.io workflow key for
 * the minispeedcam.com endpoints, not a per-device secret.
 */

#define HOSTNAME "MiniSpeedCam"  // mDNS name and Soft-AP SSID

/**
 * First-boot WiFi bring-up.
 *
 * Attempts STA mode using credentials previously stored in NVS. If that
 * fails within ~7 seconds, falls back to AP mode so the user can open
 * the ESPUI portal at http://192.168.4.1 and enter credentials.
 */
void connectWifiAP() {
  int connect_timeout;

  WiFi.setHostname(HOSTNAME);
  Serial.println("Connecting WiFi/AP");
  //Try to connect with stored credentials, fire up an access point if they don't work.
  WiFi.mode(WIFI_STA);
  WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE, INADDR_NONE);
  WiFi.begin(ssid, password);
  connect_timeout = 28;  //7 seconds
  while (WiFi.status() != WL_CONNECTED && connect_timeout > 0) {
    delay(250);
    Serial.print(".");
    connect_timeout--;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(WiFi.localIP());
    Serial.println("Wifi started");

    if (!MDNS.begin(HOSTNAME)) {
      Serial.println("Error setting up MDNS responder!");
    }

  } else {
    Serial.println("\nCreating access point...");
    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
    WiFi.softAP(HOSTNAME);

    // Resolve every host to the soft-AP IP so phones/laptops detect the
    // captive portal and the user lands on the ESPUI page automatically.
    // Paired with dnsServer.processNextRequest() in taskCore1.
    dnsServer.start(53, "*", IPAddress(192, 168, 4, 1));

    connect_timeout = 20;
    do {
      delay(250);
      Serial.print(",");
      connect_timeout--;
    } while (connect_timeout);
  }
}

/**
 * Reconnect to the configured STA network.
 *
 * Called from Core 0 when Core 1 sets `connect_wifi = true`, which
 * happens any time radar activity is detected while WiFi is down (we
 * disconnect during the post-boot grace and idle sleep windows to save
 * power). Times out at roughly 10 seconds.
 */
void connectWifi() {

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Already connected WiFi");
    return;
  }

  Serial.println("Connecting WiFi");
  WiFi.setSleep(false);  // TEST
  WiFi.mode(WIFI_STA);
  WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE, INADDR_NONE);
  WiFi.setHostname(hostname.c_str());

  // try to connect to existing network
  Serial.println("\n\nTry to connect to existing network");
  Serial.println(ssid);
  Serial.println(password);
  WiFi.begin(ssid, password);
  uint8_t timeout = 100;

  // Wait for connection, 5s timeout
  while (timeout && WiFi.status() != WL_CONNECTED) {
    delay(100);
    Serial.println(".");
    timeout--;
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Connection Failed");
  }

  Serial.println("IP address: ");
  local_ip_address = WiFi.localIP().toString();
  Serial.println(local_ip_address);
}

/**
 * Tear down the WiFi radio entirely.
 *
 * Used by sleep paths where we want the radio off until the next radar
 * trigger, primarily to drop average power consumption.
 */
void disconnectWifi() {
  Serial.println("Disconnecting wifi");
  WiFi.disconnect();
  WiFi.mode(WIFI_OFF);
  WiFi.setSleep(true);
}

/**
 * Service the WiFi reset push-button.
 *
 * Active-low button on WIFI_RESET_PIN. Polled once per Core 1 iteration:
 * if held for ~3 seconds, the stored SSID/password in NVS are overwritten
 * with placeholder values and the ESP32 reboots, which causes the next
 * connectWifiAP() to fall through to soft-AP mode for reconfiguration.
 */
void wifiResetButton() {
  if (digitalRead(WIFI_RESET_PIN) == LOW) {    // Button is pressed (LOW due to pull-up)
    delay(3000);                               // delay 3 seconds
    if (digitalRead(WIFI_RESET_PIN) == LOW) {  // Confirm button press
      Serial.println("Reset button pressed. Resetting Wi-Fi...");
      // Match the sentinel used by buttonClearNetworkCall so connectWifiAP
      // takes the AP-fallback path on next boot.
      preferences.putString("ssid", "NOT_SET");
      preferences.putString("pass", "NOT_SET");
      ESP.restart();
    }
  }
}

/**
 * Announce this device's LAN IP to the cloud.
 *
 * The minispeedcam.com web app uses the reported IP to render a "Open
 * device UI" link in the user's dashboard. Only fires when we are in
 * STA mode and connected; otherwise it returns silently.
 */
void sendLocalIP() {
  Serial.println("Sending Local IP address");

  //connectWifi();
  
  if (WiFi.getMode() != WIFI_STA) {
    Serial.println("Not in STATION MODE");
    return;
  }

  //Check WiFi connection status
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Sending API");
    // set secure client without certificate

    Serial.println("IP address: ");
    local_ip_address = WiFi.localIP().toString();
    Serial.println(local_ip_address);

    WiFiClientSecure* client = new WiFiClientSecure;
    client->setInsecure();

    //WiFiClient client;
    HTTPClient https;

    String recv_token = "bc6d8bd23bbeb6b13fa67448c244a129";  // Complete Bearer token
    recv_token = "Bearer " + recv_token;                     // Adding "Bearer " before token

    // Sending POST request
    https.begin(*client, server_local_ip_address);
    https.addHeader("Authorization", recv_token);         // Adding Bearer token as HTTP header
    https.addHeader("Content-Type", "application/json");  // Adding Bearer token as HTTP header

    httpsRequestData = "{\"camera\":\"" + String(camera_id) + "\",\"ip_address\":\"" + String(local_ip_address) + "\"}";

    Serial.println(httpsRequestData);

    // Send HTTPS POST request
    httpsResponseCode = https.POST(httpsRequestData);
    Serial.print("HTTP Response code: ");
    Serial.println(httpsResponseCode);

    if (httpsResponseCode > 0) {
      payload = https.getString();
      Serial.println(payload);
    }
    // Free resources
    https.end();
    delete client;
  }
}


/**
 * Upload the most recent capture to minispeedcam.com.
 *
 * Two endpoints are used depending on whether the run exceeded
 * `photo_speed`:
 *   - server_speeding:     full payload including base64 JPEG.
 *   - server_non_speeding: speed-only payload (no photo).
 *
 * Blocks until Core 1 finishes computing the per-vehicle max speed
 * (`speed_collection_complete`) so we always upload the highest sample
 * rather than the speed at trigger time. Runs on Core 0 and is gated
 * by `send_data` from Core 1.
 */
void sendPhoto(void) {

  Serial.println("sendPhoto");

  sending_data = true;

  if (WiFi.getMode() != WIFI_STA) {
    Serial.println("Not in STATION MODE");
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Not connected to WiFi");
    connectWifi();
  }

  uint8_t timeout = 100;

  // Wait for connection, 5s timeout
  while (timeout && WiFi.status() != WL_CONNECTED) {
    delay(100);
    Serial.println(".");
    timeout--;
  }

  // set secure client without certificate
  WiFiClientSecure* client = new WiFiClientSecure;
  client->setInsecure();

  //WiFiClient client;
  HTTPClient https;

  String recv_token = "bc6d8bd23bbeb6b13fa67448c244a129";  // Complete Bearer token
  recv_token = "Bearer " + recv_token;                     // Adding "Bearer " before token

  // Delay sending of photo until the maxSpeed data is received
  while (speed_collection_complete == false) {
    delay(100);
    Serial.println("waiting on data");
  }
  // Round to nearest int so the wire format stays "25" rather than "25.70"
  // (Arduino's String += float defaults to two decimal places).
  speed_actual = (int)(maxSpeed + 0.5f);

  String httpsRequestSend;

  // Sending POST request depending if car was speeding or not
  if (send_photo == true) {
    String send_photo_text = "true";
    https.begin(*client, server_speeding);
    https.setConnectTimeout(5000);  // set time out
    https.setTimeout(5000);
    https.addHeader("Authorization", recv_token);         // Adding Bearer token as HTTP header
    https.addHeader("Content-Type", "application/json");  // Adding Bearer token as HTTP header

    // Reserve up the request buffer once: a UXGA JPEG base64-encodes to
    // roughly 100KB+, and without this the String reallocates many times.
    httpsRequestSend.reserve(150000);
    httpsRequestSend = "{\"send_photo\":\"";
    httpsRequestSend += send_photo_text;
    httpsRequestSend += "\",\"camera\":\"";
    httpsRequestSend += camera_id;
    httpsRequestSend += "\",\"speed_actual\":\"";
    httpsRequestSend += speed_actual;
    httpsRequestSend += "\",\"photo\":{\"filename\":\"";
    httpsRequestSend += photo_filename;
    httpsRequestSend += "\",\"contents\":\"";
    httpsRequestSend += photo_base64;
    httpsRequestSend += "\"}}";

  } else {
    String send_photo_text = "false";
    https.begin(*client, server_non_speeding);
    https.setConnectTimeout(5000);  // set time out
    https.setTimeout(5000);
    https.addHeader("Authorization", recv_token);         // Adding Bearer token as HTTP header
    https.addHeader("Content-Type", "application/json");  // Adding Bearer token as HTTP header

    httpsRequestSend.reserve(1000);
    httpsRequestSend = "{\"send_photo\":\"";
    httpsRequestSend += send_photo_text;
    httpsRequestSend += "\",\"camera\":\"";
    httpsRequestSend += camera_id;
    httpsRequestSend += "\",\"speed_actual\":\"";
    httpsRequestSend += speed_actual;
    httpsRequestSend += "\"}";
  }

  Serial.println("Sending POST Request");
  httpsResponseCode = https.POST(httpsRequestSend);
  Serial.print("HTTP Response code: ");
  Serial.println(httpsResponseCode);

  if (httpsResponseCode > 0) {
    payload = https.getString();
    Serial.println(payload);
  }

  // Free resources
  https.end();
  delete client;

  // Release the ~100KB base64 buffer back to the heap; otherwise it lingers
  // until the next capture overwrites it.
  photo_base64 = String();

  //Don't disconnect wifi during first two minutes of bootup
  if (wake_flag == false) {
    //disconnectWifi(); // Don't disconnect, just go to sleep
  }

  sending_data = false;
}