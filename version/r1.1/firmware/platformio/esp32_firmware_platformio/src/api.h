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

#define HOSTNAME "MiniSpeedCam"  // base name; deviceHostname() appends a per-unit MAC suffix

/**
 * Per-device network name: HOSTNAME plus the last 3 bytes of the factory MAC,
 * e.g. "MiniSpeedCam-3CAB1F". Used for the mDNS name, the STA hostname and the
 * soft-AP SSID so two MiniSpeedCams can share one network without colliding -- a
 * fixed name makes both claim "MiniSpeedCam.local" and mDNS renames one. The
 * efuse MAC is read once and cached and needs no WiFi to be up. Only the low 3
 * bytes (the NIC-specific half) are used: the high 3 are Espressif's OUI,
 * identical on every ESP32, so they can't tell units apart.
 */
static const char* deviceHostname() {
  static char name[24] = {0};
  if (name[0] == '\0') {
    uint64_t mac = ESP.getEfuseMac();  // 48-bit factory MAC, byte 0 in the LSB
    snprintf(name, sizeof(name), "%s-%02X%02X%02X", HOSTNAME,
             (uint8_t)(mac >> 24), (uint8_t)(mac >> 32), (uint8_t)(mac >> 40));
  }
  return name;
}

// WiFi transmit power. The default (~19.5dBm) maximises range but the TX
// current spikes / RF energy couple into the CDM324 radar front-end and
// corrupt the FFT speed readings. Lowering it trades range for far less EMI;
// raise back toward WIFI_POWER_19_5dBm if WiFi range becomes the problem.
#define WIFI_TX_POWER WIFI_POWER_11dBm

// --- Radar-blanking handshake (ESP RADAR_BLANK_PIN / GPIO2 -> STM32 PA4) ------
// Held HIGH while the ESP is mid WiFi burst (upload, (re)connect). The STM32
// discards any FFT frame it sees during that window, since the TX current
// slumps the shared rail and corrupts the radar read. A depth counter makes
// nested asserts safe (e.g. sendUpload() -> connectWifi()): the line only drops
// when the outermost scope exits. Use RadarBlankGuard so every return path
// clears it automatically.
static volatile int radarBlankDepth = 0;
static inline void radarBlank(bool on) {
  if (on) {
    if (radarBlankDepth++ == 0) digitalWrite(RADAR_BLANK_PIN, HIGH);
  } else if (radarBlankDepth > 0 && --radarBlankDepth == 0) {
    digitalWrite(RADAR_BLANK_PIN, LOW);
  }
}
struct RadarBlankGuard {
  RadarBlankGuard() { radarBlank(true); }
  ~RadarBlankGuard() { radarBlank(false); }
};

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
  RadarBlankGuard _blank;  // the whole STA/AP bring-up transmits; blank the radar

  WiFi.setHostname(deviceHostname());
  Serial.println("Connecting WiFi/AP");
  // Skip the STA attempt entirely when there are no stored credentials (a fresh
  // device, or one just WiFi-reset to "NOT_SET"): trying to join a non-existent
  // network only burns ~7s before falling back to the AP. Go straight to config.
  bool have_creds = (ssid != "NOT_SET" && !ssid.isEmpty());
  if (have_creds) {
    //Try to connect with stored credentials, fire up an access point if they don't work.
    WiFi.mode(WIFI_STA);
    WiFi.setTxPower(WIFI_TX_POWER);  // reduce EMI into the radar front-end
    WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE, INADDR_NONE);
    WiFi.begin(ssid, password);
    connect_timeout = 28;  //7 seconds
    while (WiFi.status() != WL_CONNECTED && connect_timeout > 0) {
      delay(250);
      Serial.print(".");
      connect_timeout--;
    }
  } else {
    Serial.println("No stored credentials -> starting config AP directly");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(WiFi.localIP());
    Serial.println("Wifi started");

    if (!MDNS.begin(deviceHostname())) {
      Serial.println("Error setting up MDNS responder!");
    } else {
      // Log the per-device mDNS address so it's discoverable from serial (the
      // STA path otherwise prints only the IP); two units differ by MAC suffix.
      Serial.printf("[WIFI] mDNS: http://%s.local/\n", deviceHostname());
    }

  } else {
    Serial.println("\nCreating access point...");

    // Cleanly tear down the failed STA attempt before bringing the radio up
    // as an AP. Going through WIFI_OFF first avoids leaving the driver in the
    // half-configured state that can make softAP() silently fail to start.
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(100);
    WiFi.mode(WIFI_AP);
    WiFi.setTxPower(WIFI_TX_POWER);  // reduce EMI into the radar front-end

    bool cfg = WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
    if (!cfg) Serial.println("[AP] softAPConfig() failed");

    // softAP() can transiently fail right after a STA teardown; retry a few
    // times rather than booting with no portal and no indication why.
    bool ap_up = false;
    for (int attempt = 1; attempt <= 3 && !ap_up; attempt++) {
      ap_up = WiFi.softAP(deviceHostname());  // open AP (no password) named after the device
      if (!ap_up) {
        Serial.printf("[AP] softAP() failed, retry %d/3\n", attempt);
        delay(500);
      }
    }

    if (ap_up) {
      Serial.printf("[AP] up: SSID=\"%s\" ip=%s\n", deviceHostname(), WiFi.softAPIP().toString().c_str());
    } else {
      Serial.println("[AP] FAILED to start access point");
    }
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

  // No credentials stored yet: the device is intentionally in AP config mode.
  // Bail BEFORE switching to STA so we don't tear down the soft-AP that
  // connectWifiAP() brought up (and that the user needs to enter creds).
  if (ssid == "NOT_SET" || ssid.isEmpty()) {
    Serial.println("No WiFi credentials stored; staying in AP mode");
    return;
  }

  RadarBlankGuard _blank;  // the association/auth burst slumps the shared rail
  Serial.println("Connecting WiFi");
  WiFi.setSleep(false);  // TEST
  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_TX_POWER);  // reduce EMI into the radar front-end
  WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE, INADDR_NONE);
  WiFi.setHostname(deviceHostname());  // match connectWifiAP()/mDNS so the station hostname stays consistent across reconnects

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
 * Active-low button on WIFI_RESET_PIN. Polled once per Core 1 iteration.
 * Non-blocking: instead of stalling the loop with delay(3000), it records
 * when a continuous press began and fires only once the button has been
 * held LOW for >= WIFI_RESET_HOLD_MS. Releasing the button (pin returns
 * HIGH) clears the timer, so the 3 seconds must be a single sustained
 * hold rather than the sum of brief taps.
 *
 * On trigger, the stored SSID/password in NVS are overwritten with
 * placeholder values and the ESP32 reboots, which causes the next
 * connectWifiAP() to fall through to soft-AP mode for reconfiguration.
 */
void wifiResetButton() {
  static const unsigned long WIFI_RESET_HOLD_MS = 3000;
  static unsigned long pressStart = 0;  // millis() when the current hold began; 0 = not pressed

  if (digitalRead(WIFI_RESET_PIN) == LOW) {  // Button is pressed (LOW due to pull-up)
    unsigned long now = millis();
    if (pressStart == 0) {
      pressStart = now ? now : 1;  // start timing; avoid 0 (the "not pressed" sentinel) on the millis() rollover tick
      Serial.println("[RESET] button DOWN on GPIO21 (hold 3s to clear WiFi creds)");
    } else if (now - pressStart >= WIFI_RESET_HOLD_MS) {
      Serial.println("Reset button held 3s. Resetting Wi-Fi...");
      // Restore the unconfigured sentinel "NOT_SET" (NOT the literal "ssid"/"pass":
      // those look like real credentials, so taskCore1 keeps requesting reconnects
      // and connectWifi() tears the soft-AP back down to retry a bogus network,
      // looping forever). connectWifi() and the taskCore1 reconnect guard both bail
      // on "NOT_SET", so the device stays cleanly in AP mode for reconfiguration.
      preferences.putString("ssid", "NOT_SET");
      preferences.putString("pass", "NOT_SET");
      ESP.restart();
    }
  } else {
    if (pressStart != 0) {
      Serial.printf("[RESET] button UP after %lums (need >=3000 to reset)\n",
                    (unsigned long) (millis() - pressStart));
    }
    pressStart = 0;  // released before the threshold; reset so taps don't accumulate
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

  // Keyed by camera on the server; "NOT_SET" has no record, so don't bother.
  if (camera_id == "NOT_SET" || camera_id.isEmpty()) {
    Serial.println("camera_id not configured; skipping IP announce");
    return;
  }

  if (WiFi.getMode() != WIFI_STA) {
    Serial.println("Not in STATION MODE");
    return;
  }

  //Check WiFi connection status
  if (WiFi.status() == WL_CONNECTED) {
    RadarBlankGuard _blank;  // blank the radar for the IP-announce POST
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

  // No camera configured: the server has no record for "NOT_SET" and rejects
  // the POST with 400 MISSING_DATA. Skip the upload rather than burning a
  // connection + TLS handshake on a request that can only fail.
  if (camera_id == "NOT_SET" || camera_id.isEmpty()) {
    Serial.println("[UPLOAD] skipped: camera_id not configured");
    return;
  }

  // Client-side rate limit: never POST capture events closer than 3s apart.
  // The Bubble server enforces this authoritatively; this just keeps a burst
  // of back-to-back passes from tripping the server limit during normal use.
  // Events inside the window are dropped (not queued) so Core 0 never blocks.
  static const unsigned long MIN_UPLOAD_INTERVAL_MS = 3000;
  static unsigned long lastUploadMs = 0;
  static bool haveUploaded = false;
  if (haveUploaded && (millis() - lastUploadMs) < MIN_UPLOAD_INTERVAL_MS) {
    Serial.printf("[UPLOAD] rate-limited: dropping event (%lums since last)\n",
                  (unsigned long)(millis() - lastUploadMs));
    return;
  }

  if (WiFi.getMode() != WIFI_STA) {
    Serial.println("Not in STATION MODE");
    return;
  }

  // Blank the radar for the (possible) reconnect + the POST below; the guard
  // clears it on every return path. connectWifi() nests its own guard safely.
  RadarBlankGuard _blank;

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

  // Commit the rate-limit timestamp now that we're actually sending.
  lastUploadMs = millis();
  haveUploaded = true;

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