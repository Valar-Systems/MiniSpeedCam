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
 *   - sendUpload():       POST a finished run to the cloud — streams the
 *                        captured photo + max speed, or just the speed
 *                        when no photo was captured.
 *
 * The Bearer token and optional TLS root certificate come from config.h
 * (both build-flag overridable); see that file for details.
 */

#include "config.h"

#define HOSTNAME "MiniSpeedCam"  // mDNS name and Soft-AP SSID

/**
 * Apply the TLS trust policy to a secure client.
 *
 * Pins against the configured CA root (config.h / API_CA_ROOT_CERT) when one
 * is provided; otherwise falls back to setInsecure() (no certificate
 * validation) to preserve the original behaviour out of the box.
 */
static void applyTlsPolicy(WiFiClientSecure& client) {
  if (kCaRootCert != nullptr && kCaRootCert[0] != '\0') {
    client.setCACert(kCaRootCert);
  } else {
    client.setInsecure();
  }
}

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
  WiFi.setHostname(HOSTNAME);  // match connectWifiAP()/mDNS so the station hostname stays "MiniSpeedCam" across reconnects

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
      preferences.putString("ssid", "ssid");  // This replaces the stored wifi network with a random value
      preferences.putString("pass", "pass");  // This replaces the stored wifi network with a random value
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

    WiFiClientSecure client;  // stack-scoped: freed on return, no leak (lives past https.end())
    applyTlsPolicy(client);

    HTTPClient https;

    String recv_token = "Bearer " API_BEARER_TOKEN;  // token from config.h (build-flag overridable)

    // Sending POST request
    https.begin(client, server_local_ip_address);
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


/**
 * StreamingUploadBody - an Arduino Stream that synthesises the JSON upload
 * body on the fly so the base64-encoded JPEG is never materialised in heap.
 *
 * HTTPClient::sendRequest(type, Stream*, size) pulls the body in ~1.4 KB
 * chunks via available()/readBytes(); we emit the JSON prologue, then base64
 * of the camera framebuffer encoded one quad at a time straight from PSRAM,
 * then the JSON epilogue. Peak heap is the two short prologue/epilogue
 * Strings instead of the ~300 KB the old String-concatenation path required.
 */
class StreamingUploadBody : public Stream {
public:
  StreamingUploadBody(const String& prologue, const uint8_t* data, size_t dataLen, const String& epilogue)
    : _prologue(prologue), _epilogue(epilogue), _data(data), _dataLen(dataLen),
      _prologueLen(prologue.length()), _epilogueLen(epilogue.length()), _pos(0) {
    _b64Len = 4 * ((dataLen + 2) / 3);  // standard padded base64 length, no line breaks
    _total = _prologueLen + _b64Len + _epilogueLen;
  }

  size_t totalSize() const { return _total; }

  int available() override {
    size_t remaining = _total - _pos;
    return remaining > 0x7fffffff ? 0x7fffffff : (int)remaining;
  }

  // Fill `buffer` with up to `length` body bytes, walking the prologue ->
  // base64 -> epilogue regions. This is the call HTTPClient uses to stream.
  size_t readBytes(char* buffer, size_t length) override {
    size_t produced = 0;
    while (produced < length && _pos < _total) {
      size_t want = length - produced;
      if (_pos < _prologueLen) {
        size_t avail = _prologueLen - _pos;
        size_t chunk = want < avail ? want : avail;
        memcpy(buffer + produced, _prologue.c_str() + _pos, chunk);
        produced += chunk;
        _pos += chunk;
      } else if (_pos < _prologueLen + _b64Len) {
        size_t b64off = _pos - _prologueLen;
        size_t avail = _b64Len - b64off;
        size_t chunk = want < avail ? want : avail;
        encodeRange(b64off, chunk, (uint8_t*)buffer + produced);
        produced += chunk;
        _pos += chunk;
      } else {
        size_t eoff = _pos - _prologueLen - _b64Len;
        size_t avail = _epilogueLen - eoff;
        size_t chunk = want < avail ? want : avail;
        memcpy(buffer + produced, _epilogue.c_str() + eoff, chunk);
        produced += chunk;
        _pos += chunk;
      }
    }
    return produced;
  }

  int read() override {
    char b;
    return readBytes(&b, 1) == 1 ? (uint8_t)b : -1;
  }

  int peek() override { return -1; }            // HTTPClient never peeks the upload stream
  size_t write(uint8_t) override { return 0; }  // read-only stream

private:
  // Emit `count` base64 characters starting at output offset `b64off`,
  // computing each 4-char quad from the corresponding 3 source bytes.
  void encodeRange(size_t b64off, size_t count, uint8_t* dest) {
    static const char tbl[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i = 0;
    size_t outpos = b64off;
    while (i < count) {
      size_t quad = outpos / 4;
      size_t within = outpos % 4;
      size_t inIdx = quad * 3;
      uint8_t b0 = inIdx < _dataLen ? _data[inIdx] : 0;
      uint8_t b1 = (inIdx + 1) < _dataLen ? _data[inIdx + 1] : 0;
      uint8_t b2 = (inIdx + 2) < _dataLen ? _data[inIdx + 2] : 0;
      char c[4];
      c[0] = tbl[b0 >> 2];
      c[1] = tbl[((b0 & 0x03) << 4) | (b1 >> 4)];
      c[2] = (inIdx + 1) < _dataLen ? tbl[((b1 & 0x0f) << 2) | (b2 >> 6)] : '=';
      c[3] = (inIdx + 2) < _dataLen ? tbl[b2 & 0x3f] : '=';
      while (within < 4 && i < count) {
        dest[i++] = (uint8_t)c[within++];
        outpos++;
      }
    }
  }

  String _prologue;
  String _epilogue;
  const uint8_t* _data;
  size_t _dataLen;
  size_t _prologueLen;
  size_t _epilogueLen;
  size_t _b64Len;
  size_t _total;
  size_t _pos;
};

/**
 * Upload one capture event to minispeedcam.com (runs on Core 0).
 *
 * Always POSTs to the single `server_capture` endpoint. When the event
 * carries a photo (req.has_photo / req.fb) the JPEG is base64-streamed on
 * the fly straight from the PSRAM framebuffer via StreamingUploadBody, so
 * the image is never duplicated in heap; otherwise a short speed-only JSON
 * body is sent. The `send_photo` field tells the server which case it is.
 *
 * The caller owns req.fb and must esp_camera_fb_return() it after this
 * returns. speed_actual is already final (the run has ended), so there is
 * no waiting/spinning here.
 */
void sendUpload(const UploadRequest& req) {

  Serial.printf("[UPLOAD] start: %s, heap=%u\n",
                (req.has_photo && req.fb != nullptr) ? "photo" : "speed-only",
                (unsigned)ESP.getFreeHeap());

  if (WiFi.getMode() != WIFI_STA) {
    Serial.println("Not in STATION MODE");
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Not connected to WiFi");
    connectWifi();
  }

  uint8_t timeout = 100;
  // Wait for connection, ~10s timeout
  while (timeout && WiFi.status() != WL_CONNECTED) {
    delay(100);
    Serial.println(".");
    timeout--;
  }

  // secure client; TLS policy per config.h (pinned cert if set, else insecure)
  WiFiClientSecure client;  // stack-scoped: freed on return, no leak (lives past https.end())
  applyTlsPolicy(client);

  HTTPClient https;
  String recv_token = "Bearer " API_BEARER_TOKEN;  // token from config.h (build-flag overridable)

  https.begin(client, server_capture);
  https.setConnectTimeout(5000);  // set time out
  https.setTimeout(5000);
  https.addHeader("Authorization", recv_token);         // Adding Bearer token as HTTP header
  https.addHeader("Content-Type", "application/json");  // Adding Bearer token as HTTP header

  if (req.has_photo && req.fb != nullptr) {
    // Stream prologue + base64(framebuffer) + epilogue without ever holding
    // the whole encoded image in RAM.
    String prologue = "{\"send_photo\":\"true\",\"camera\":\"";
    prologue += camera_id;
    prologue += "\",\"speed_actual\":\"";
    prologue += req.speed_actual;
    prologue += "\",\"photo\":{\"filename\":\"image.jpg\",\"contents\":\"";
    String epilogue = "\"}}";

    StreamingUploadBody body(prologue, req.fb->buf, req.fb->len, epilogue);
    Serial.printf("[UPLOAD] streaming POST, body=%u bytes\n", (unsigned)body.totalSize());
    httpsResponseCode = https.sendRequest("POST", &body, body.totalSize());
  } else {
    // Speed-only event (no photo): short, fully-buffered JSON body.
    String json = "{\"send_photo\":\"false\",\"camera\":\"";
    json += camera_id;
    json += "\",\"speed_actual\":\"";
    json += req.speed_actual;
    json += "\"}";
    Serial.println("[UPLOAD] speed-only POST");
    httpsResponseCode = https.POST(json);
  }

  Serial.printf("[UPLOAD] HTTP %d %s\n", httpsResponseCode,
                (httpsResponseCode >= 200 && httpsResponseCode < 300) ? "PASS" : "FAIL");
  diagnosticsRecordUpload(httpsResponseCode);  // surface the result on the ESPUI Status tab

  if (httpsResponseCode > 0) {
    payload = https.getString();
    Serial.println(payload);
  }

  // Free resources
  https.end();
  Serial.printf("[UPLOAD] done, heap=%u\n", (unsigned)ESP.getFreeHeap());
}