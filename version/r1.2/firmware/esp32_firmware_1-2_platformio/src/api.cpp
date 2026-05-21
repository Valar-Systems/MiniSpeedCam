#include "api.h"

#include <time.h>

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ESPmDNS.h>
#include <DNSServer.h>

#include "camera.h"
#include "diagnostics.h"
#include "ota.h"
#include "pins.h"
#include "queue.h"
#include "variables.h"

// Captive-DNS server owned by main.cpp; used here to redirect every host
// lookup to the soft-AP gateway IP when no STA credentials are stored.
extern DNSServer dnsServer;

// Cloud endpoints (Bubble.io workflow URLs on minispeedcam.com). Only
// referenced from this translation unit.
//
// kCaptureEndpoint accepts every speed event the firmware uploads -- it
// always carries the JPEG today, so there is no separate "no-photo"
// path. (The earlier /speeding_capture and /non_speeding_capture
// workflows were collapsed into this single one.)
static const char* kCaptureEndpoint      = "https://minispeedcam.com/api/1.1/wf/capture";
static const char* kServerLocalIpAddress = "https://minispeedcam.com/api/1.1/wf/local_ip_address";

// Bearer token for the public Bubble.io workflow endpoints.
static const char* kBearerToken = "bc6d8bd23bbeb6b13fa67448c244a129";

// Root CA certificate pinning. When this string is non-empty,
// WiFiClientSecure.setCACert() is used instead of setInsecure(),
// which validates the server's certificate chain against this anchor.
//
// To enable: paste the PEM-encoded root that signs minispeedcam.com
// here (Cloudflare's "Baltimore CyberTrust Root" or whichever the
// Bubble app currently uses -- check with `openssl s_client -connect
// minispeedcam.com:443 -showcerts`). Format the PEM as a C string
// literal with newlines as \n. Leaving this empty preserves the
// previous (insecure) behaviour.
static const char* kCaRootCert = "";

// Minimum gap between event uploads. The radar is unlikely to produce
// genuinely-distinct events at sub-second cadence, and rate-limiting
// here defends the cloud quota if the radar ever wedges high.
static constexpr unsigned long kMinUploadGapMs = 3000;

// Apply CA pinning if configured, otherwise fall back to setInsecure.
static void configureTls(WiFiClientSecure* client) {
  if (kCaRootCert && kCaRootCert[0] != '\0') {
    client->setCACert(kCaRootCert);
  } else {
    client->setInsecure();
  }
}

// Kick off SNTP using two well-known servers, UTC offset. Safe to call
// multiple times -- subsequent calls just re-prime the resolver. The
// initial sync is async; time() returns epoch < 1700000000 until the
// first response lands (typically a few seconds after the call).
static void startNtpSync() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
}

// Epoch seconds when the system clock is at least plausibly synced
// (post-2023), otherwise 0 so the server can fall back to receive-time.
static long long currentEpochOrZero() {
  time_t now = time(nullptr);
  return (now > 1700000000) ? (long long)now : 0LL;
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

    startNtpSync();  // Kick off clock sync so event_time gets a real epoch
    otaBegin();      // Start ArduinoOTA listener on the freshly-joined network

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

  // Refresh the ArduinoOTA listener -- the previous UDP socket was torn
  // down when WiFi dropped, so begin() needs to run again on each
  // successful reconnect for OTA to remain reachable. Same logic for
  // SNTP: the resolver state was lost during the disconnect window.
  if (WiFi.status() == WL_CONNECTED) {
    startNtpSync();
    otaBegin();
    // Drain any events that we queued offline while WiFi was down.
    queueDrain();
  }
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
  configureTls(client);

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
 * Stateless event upload. Builds the JSON wrapper around the JPEG and
 * streams it straight from `jpeg` (PSRAM, heap, or a file-backed buffer
 * -- the caller decides) so nothing the size of the encoded photo ever
 * sits in regular heap.
 */
int apiUploadEventFromMemory(int speed,
                             long long epoch,
                             const String& camera_id,
                             const uint8_t* jpeg,
                             size_t jpeg_len) {
  if (WiFi.status() != WL_CONNECTED) {
    return -1;
  }

  WiFiClientSecure* client = new WiFiClientSecure;
  configureTls(client);

  HTTPClient https;
  https.begin(*client, kCaptureEndpoint);
  https.setConnectTimeout(5000);
  https.setTimeout(15000);

  String token = String("Bearer ") + kBearerToken;
  https.addHeader("Authorization", token);
  https.addHeader("Content-Type", "application/json");

  // "send_photo" is hardcoded "true" -- preserved in the payload so the
  // Bubble workflow can keep the same field shape it had under the old
  // /speeding_capture URL.
  String prologue;
  prologue.reserve(220);
  prologue  = "{\"send_photo\":\"true\",\"camera\":\"";
  prologue += camera_id;
  prologue += "\",\"speed_actual\":\"";
  prologue += speed;
  prologue += "\",\"event_time\":\"";
  prologue += String((long long)epoch);
  prologue += "\",\"photo\":{\"filename\":\"image.jpg\",\"contents\":\"";

  const String epilogue = "\"}}";

  StreamingUploadBody body(prologue, jpeg, jpeg_len, epilogue);

  Serial.printf("Streaming POST, payload bytes: %u\n",
                (unsigned)body.totalLength());
  int http_code = https.sendRequest("POST", &body, body.totalLength());
  Serial.printf("HTTP Response code: %d\n", http_code);

  if (http_code > 0) {
    Serial.println(https.getString());
  }

  https.end();
  delete client;
  return http_code;
}

/**
 * Upload the most recent capture to minispeedcam.com.
 *
 * Wraps apiUploadEventFromMemory() with the live-capture lifecycle:
 * waits for Core 1 to finalize maxSpeed, enforces the per-event rate
 * limit, records the HTTP outcome for the Status tab, and -- if the
 * POST cannot go out cleanly -- hands the JPEG off to the offline
 * queue so it can be retried later instead of being dropped on the
 * floor.
 */
void sendPhoto() {
  Serial.println("sendPhoto");

  // Order matters: photo_filename is cleared *before* releasing the
  // framebuffer because once g_pending_fb goes to nullptr Core 1 is
  // free to enter takePhoto() and write a new filename -- doing it the
  // other way around would race.
  auto releasePhoto = []() {
    upload.photo_filename = String();
    cameraReleasePendingPhoto();
  };

  // Defensive: send_data should never be raised without a pending fb,
  // but if it ever is we want to release whatever state is around and
  // not POST a half-built payload.
  if (!cameraHasPendingPhoto()) {
    Serial.println("sendPhoto called with no pending framebuffer; aborting");
    releasePhoto();
    return;
  }

  // Hold until Core 1 has finalized maxSpeed for the run.
  while (upload.speed_collection_complete == false) {
    delay(100);
    Serial.println("waiting on data");
  }
  const int speed_actual = radar.maxSpeed;
  const long long event_time = currentEpochOrZero();

  // Rate-limit: distinct radar events should never arrive sub-second; a
  // burst suggests a wedged signal. Queue rather than POST.
  static unsigned long s_last_upload_ms = 0;
  const bool rate_limited =
      (s_last_upload_ms != 0) &&
      (millis() - s_last_upload_ms < kMinUploadGapMs);

  if (rate_limited) {
    Serial.println("Rate-limited; queueing event for later");
    queueEnqueueEvent(speed_actual, event_time, net.camera_id,
                      cameraPendingPhotoData(),
                      cameraPendingPhotoLength());
    releasePhoto();
    return;
  }

  // If WiFi is down, queue instead of dropping. Try a quick reconnect
  // first so transient blips don't push every event to disk.
  if (WiFi.getMode() != WIFI_STA) {
    Serial.println("Not in STATION MODE; queueing event");
    queueEnqueueEvent(speed_actual, event_time, net.camera_id,
                      cameraPendingPhotoData(),
                      cameraPendingPhotoLength());
    releasePhoto();
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected; attempting reconnect before upload");
    connectWifi();
    uint8_t timeout = 50;  // ~5s
    while (timeout && WiFi.status() != WL_CONNECTED) {
      delay(100);
      timeout--;
    }
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Upload deferred: WiFi did not come back; queueing");
    queueEnqueueEvent(speed_actual, event_time, net.camera_id,
                      cameraPendingPhotoData(),
                      cameraPendingPhotoLength());
    releasePhoto();
    return;
  }

  s_last_upload_ms = millis();
  int http_code = apiUploadEventFromMemory(
      speed_actual, event_time, net.camera_id,
      cameraPendingPhotoData(),
      cameraPendingPhotoLength());

  diagnosticsRecordUpload(http_code);

  // Anything outside 2xx is treated as a delivery failure. Queue for
  // retry on the next drain.
  if (http_code < 200 || http_code >= 300) {
    Serial.printf("Upload failed (HTTP %d); queueing event for retry\n",
                  http_code);
    queueEnqueueEvent(speed_actual, event_time, net.camera_id,
                      cameraPendingPhotoData(),
                      cameraPendingPhotoLength());
  }

  releasePhoto();
}
