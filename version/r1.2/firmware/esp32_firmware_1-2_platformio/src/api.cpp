#include "api.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ESPmDNS.h>
#include <DNSServer.h>

#include "camera.h"
#include "pins.h"
#include "variables.h"

// Captive-DNS server owned by main.cpp; used here to redirect every host
// lookup to the soft-AP gateway IP when no STA credentials are stored.
extern DNSServer dnsServer;

// Cloud endpoints (Bubble.io workflow URLs on minispeedcam.com). Only
// referenced from this translation unit.
static const char* kServerSpeeding       = "https://minispeedcam.com/api/1.1/wf/speeding_capture";
static const char* kServerNonSpeeding    = "https://minispeedcam.com/api/1.1/wf/non_speeding_capture";
static const char* kServerLocalIpAddress = "https://minispeedcam.com/api/1.1/wf/local_ip_address";

// Bearer token for the public Bubble.io workflow endpoints.
static const char* kBearerToken = "bc6d8bd23bbeb6b13fa67448c244a129";

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
  WiFi.begin(net.ssid, net.password);
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

    // Redirect every DNS lookup to the AP's gateway IP so that any URL
    // typed by the user opens the ESPUI portal (captive portal behavior).
    dnsServer.start(53, "*", IPAddress(192, 168, 4, 1));
  }
}

/**
 * Reconnect to the configured STA network.
 *
 * Called from Core 0 when Core 1 sets `upload.connect_wifi = true`, which
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
  WiFi.setHostname(HOSTNAME);

  // try to connect to existing network
  Serial.println("\n\nTry to connect to existing network");
  Serial.println(net.ssid);
  Serial.println(net.password);
  WiFi.begin(net.ssid, net.password);
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
  net.local_ip_address = WiFi.localIP().toString();
  Serial.println(net.local_ip_address);
}

/**
 * Service the WiFi reset push-button.
 *
 * Active-low button on WIFI_RESET_PIN. Polled once per Core 1 iteration:
 * if held for ~3 seconds, the stored SSID/password in NVS are reset to
 * the "NOT_SET" sentinel and the ESP32 reboots, which causes the next
 * connectWifiAP() to fall through to soft-AP mode for reconfiguration.
 *
 * Non-blocking: the press timestamp is latched on the first low sample,
 * then the radar loop keeps running until the hold threshold elapses.
 */
void wifiResetButton() {
  static unsigned long press_start_ms = 0;
  constexpr unsigned long kHoldThresholdMs = 3000;

  if (digitalRead(WIFI_RESET_PIN) == LOW) {  // Active-low: button is held
    if (press_start_ms == 0) {
      press_start_ms = millis();
    } else if (millis() - press_start_ms >= kHoldThresholdMs) {
      Serial.println("Reset button held. Clearing Wi-Fi credentials...");
      preferences.putString("ssid", "NOT_SET");
      preferences.putString("pass", "NOT_SET");
      ESP.restart();
    }
  } else {
    press_start_ms = 0;  // Released before threshold; reset the timer.
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

  if (WiFi.getMode() != WIFI_STA) {
    Serial.println("Not in STATION MODE");
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  Serial.println("Sending API");

  Serial.println("IP address: ");
  net.local_ip_address = WiFi.localIP().toString();
  Serial.println(net.local_ip_address);

  WiFiClientSecure* client = new WiFiClientSecure;
  client->setInsecure();

  HTTPClient https;

  String recv_token = String("Bearer ") + kBearerToken;

  // Sending POST request
  https.begin(*client, kServerLocalIpAddress);
  https.addHeader("Authorization", recv_token);
  https.addHeader("Content-Type", "application/json");

  String body = "{\"camera\":\"" + net.camera_id +
                "\",\"ip_address\":\"" + net.local_ip_address + "\"}";

  Serial.println(body);

  int http_code = https.POST(body);
  Serial.print("HTTP Response code: ");
  Serial.println(http_code);

  if (http_code > 0) {
    Serial.println(https.getString());
  }

  https.end();
  delete client;
}


/**
 * Streamable JSON body that concatenates a prologue, on-the-fly base64
 * encoding of the JPEG framebuffer, and an epilogue.
 *
 * The whole point is to never materialize the encoded payload in RAM:
 * HTTPClient::sendRequest() reads ~1.4 KB at a time via readBytes() and
 * writes that directly to TCP, so the only buffers that exist are the
 * source JPEG (in PSRAM), the prologue + epilogue Strings (a few
 * hundred bytes total), and the TCP write buffer owned by HTTPClient.
 *
 * Total bytes the stream will produce are computed up front so we can
 * set Content-Length and let the receiver know when the body is done.
 */
class StreamingUploadBody : public Stream {
 public:
  StreamingUploadBody(const String& prologue,
                      const uint8_t* jpeg,
                      size_t jpeg_len,
                      const String& epilogue)
      : prologue_(prologue),
        epilogue_(epilogue),
        jpeg_(jpeg),
        jpeg_len_(jpeg_len),
        base64_chars_(((jpeg_len + 2) / 3) * 4),
        total_(prologue.length() + base64_chars_ + epilogue.length()) {}

  size_t totalLength() const { return total_; }

  // --- Stream interface (read side) ---
  int available() override { return (int)(total_ - pos_); }

  int read() override {
    if (pos_ >= total_) return -1;
    return (uint8_t)charAt(pos_++);
  }

  int peek() override {
    if (pos_ >= total_) return -1;
    return (uint8_t)charAt(pos_);
  }

  size_t readBytes(char* buffer, size_t length) override {
    size_t out = 0;
    while (out < length && pos_ < total_) {
      buffer[out++] = charAt(pos_++);
    }
    return out;
  }

  // --- Print interface (write side, unused) ---
  size_t write(uint8_t) override { return 0; }
  size_t write(const uint8_t*, size_t) override { return 0; }

 private:
  char charAt(size_t i) const {
    if (i < prologue_.length()) {
      return prologue_[i];
    }
    i -= prologue_.length();
    if (i < base64_chars_) {
      return base64CharAt(i);
    }
    i -= base64_chars_;
    return epilogue_[i];
  }

  char base64CharAt(size_t i) const {
    // Standard base64 alphabet; '=' padding for the final 1- or 2-byte group.
    static const char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const size_t group = i >> 2;     // i / 4
    const size_t slot  = i & 0x03;   // i % 4
    const size_t src   = group * 3;
    const uint8_t b0 = (src     < jpeg_len_) ? jpeg_[src]     : 0;
    const uint8_t b1 = (src + 1 < jpeg_len_) ? jpeg_[src + 1] : 0;
    const uint8_t b2 = (src + 2 < jpeg_len_) ? jpeg_[src + 2] : 0;
    switch (slot) {
      case 0: return kAlphabet[(b0 >> 2) & 0x3F];
      case 1: return kAlphabet[((b0 << 4) | (b1 >> 4)) & 0x3F];
      case 2: return (src + 1 < jpeg_len_) ? kAlphabet[((b1 << 2) | (b2 >> 6)) & 0x3F] : '=';
      case 3: return (src + 2 < jpeg_len_) ? kAlphabet[b2 & 0x3F] : '=';
    }
    return '=';
  }

  String prologue_;
  String epilogue_;
  const uint8_t* jpeg_;
  size_t jpeg_len_;
  size_t base64_chars_;
  size_t total_;
  size_t pos_ = 0;
};

/**
 * Upload the most recent capture to minispeedcam.com.
 *
 * Two endpoints are used depending on whether the run exceeded
 * `radar.photo_speed`:
 *   - kServerSpeeding:     full payload including base64 JPEG.
 *   - kServerNonSpeeding:  speed-only payload (no photo).
 *
 * The speeding branch streams the JPEG straight from the PSRAM-resident
 * framebuffer through StreamingUploadBody; nothing the size of the
 * encoded photo ever sits in regular heap.
 *
 * Blocks until Core 1 finishes computing the per-vehicle max speed
 * (`upload.speed_collection_complete`) so we always upload the highest
 * sample rather than the speed at trigger time. Runs on Core 0 and is
 * gated by `upload.send_data` from Core 1.
 */
void sendPhoto() {
  Serial.println("sendPhoto");

  // Whatever the outcome, photo_filename must be cleared and the
  // framebuffer must go back to the driver so Core 1's next takePhoto()
  // can run. Order matters: photo_filename is cleared *before* releasing
  // the framebuffer, because once g_pending_fb goes to nullptr Core 1 is
  // free to enter takePhoto() and write a new filename -- doing it the
  // other way around would race.
  auto releasePhoto = []() {
    upload.photo_filename = String();
    cameraReleasePendingPhoto();
  };

  if (WiFi.getMode() != WIFI_STA) {
    Serial.println("Not in STATION MODE");
    releasePhoto();
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Not connected to WiFi");
    connectWifi();
  }

  uint8_t timeout = 100;  // Wait for connection, 10s timeout
  while (timeout && WiFi.status() != WL_CONNECTED) {
    delay(100);
    Serial.println(".");
    timeout--;
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Upload aborted: WiFi not connected");
    releasePhoto();
    return;
  }

  WiFiClientSecure* client = new WiFiClientSecure;
  client->setInsecure();

  HTTPClient https;
  String recv_token = String("Bearer ") + kBearerToken;

  // Hold the upload until Core 1 has finalized maxSpeed for the run.
  while (upload.speed_collection_complete == false) {
    delay(100);
    Serial.println("waiting on data");
  }
  int speed_actual = radar.maxSpeed;

  if (upload.send_photo == true && cameraHasPendingPhoto()) {
    https.begin(*client, kServerSpeeding);
    https.setConnectTimeout(5000);
    https.setTimeout(15000);  // base64 upload over WiFi may exceed the 5s used for control APIs
    https.addHeader("Authorization", recv_token);
    https.addHeader("Content-Type", "application/json");

    String prologue;
    prologue.reserve(160);
    prologue  = "{\"send_photo\":\"true\",\"camera\":\"";
    prologue += net.camera_id;
    prologue += "\",\"speed_actual\":\"";
    prologue += speed_actual;
    prologue += "\",\"photo\":{\"filename\":\"";
    prologue += upload.photo_filename;
    prologue += "\",\"contents\":\"";

    const String epilogue = "\"}}";

    StreamingUploadBody body(prologue,
                             cameraPendingPhotoData(),
                             cameraPendingPhotoLength(),
                             epilogue);

    Serial.print("Streaming POST, payload bytes: ");
    Serial.println(body.totalLength());
    int http_code = https.sendRequest("POST", &body, body.totalLength());
    Serial.print("HTTP Response code: ");
    Serial.println(http_code);

    if (http_code > 0) {
      Serial.println(https.getString());
    }
  } else {
    // No photo on this event: post the speed-only payload to the
    // dedicated endpoint. Small body, no point in streaming it.
    https.begin(*client, kServerNonSpeeding);
    https.setConnectTimeout(5000);
    https.setTimeout(5000);
    https.addHeader("Authorization", recv_token);
    https.addHeader("Content-Type", "application/json");

    String body;
    body.reserve(256);
    body  = "{\"send_photo\":\"false\",\"camera\":\"";
    body += net.camera_id;
    body += "\",\"speed_actual\":\"";
    body += speed_actual;
    body += "\"}";

    Serial.println("Sending POST Request");
    int http_code = https.POST(body);
    Serial.print("HTTP Response code: ");
    Serial.println(http_code);

    if (http_code > 0) {
      Serial.println(https.getString());
    }
  }

  https.end();
  delete client;

  releasePhoto();
}
