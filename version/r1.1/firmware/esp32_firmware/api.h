#define HOSTNAME "MiniSpeedCam"

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

    connect_timeout = 20;
    do {
      delay(250);
      Serial.print(",");
      connect_timeout--;
    } while (connect_timeout);
  }
}

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

void disconnectWifi() {
  Serial.println("Disconnecting wifi");
  WiFi.disconnect();
  WiFi.mode(WIFI_OFF);
  WiFi.setSleep(true);
}

// Check if the wifi reset button is pressed. Will clear the wifi network and password, and cause the system to restart
void wifiResetButton() {
  if (digitalRead(WIFI_RESET_PIN) == LOW) {    // Button is pressed (LOW due to pull-up)
    delay(3000);                               // delay 3 seconds
    if (digitalRead(WIFI_RESET_PIN) == LOW) {  // Confirm button press
      Serial.println("Reset button pressed. Resetting Wi-Fi...");
      preferences.putString("ssid", "ssid");  // This replaces the stored wifi network with a random value
      preferences.putString("pass", "pass");  // This replaces the stored wifi network with a random value
      ESP.restart();
    }
  }
}

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
  }
}


void takeSendPhoto(void) {

  Serial.println("takeSendPhoto");

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
  speed_actual = maxSpeed;

  String httpsRequestSend;

  // Sending POST request depending if car was speeding or not
  if (send_photo == true) {
    String send_photo_text = "true";
    https.begin(*client, server_speeding);
    https.setConnectTimeout(5000);  // set time out
    https.setTimeout(5000);
    https.addHeader("Authorization", recv_token);         // Adding Bearer token as HTTP header
    https.addHeader("Content-Type", "application/json");  // Adding Bearer token as HTTP header

    httpsRequestData.reserve(150000);
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

    String httpsRequestData;
    httpsRequestData.reserve(50000);
    httpsRequestSend = "{\"send_photo\":\"";
    httpsRequestSend += send_photo_text;
    httpsRequestSend += "\",\"camera\":\"";
    httpsRequestSend += camera_id;
    httpsRequestSend += "\",\"speed_actual\":\"";
    httpsRequestSend += speed_actual;
    httpsRequestSend += "\"}}";
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

  //Don't disconnect wifi during first two minutes of bootup
  if (wake_flag == false) {
    //disconnectWifi(); // Don't disconnect, just go to sleep
  }

  sending_data = false;
}