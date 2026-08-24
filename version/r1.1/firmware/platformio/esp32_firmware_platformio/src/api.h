/**
 * api.h - WiFi management and capture uploads to the configured API base.
 *
 * Uploads default to minispeedcam.com (HTTPS) but the base URL is user-editable
 * (Wifi Settings tab -> Data Server), so the same code path serves a generic
 * endpoint such as a LAN Home Assistant webhook. selectApiClient() picks TLS vs
 * plain transport from the URL scheme, and sendUpload() accepts any 2xx from a
 * non-Bubble server (see g_server_secure / g_server_is_bubble in variables.h).
 *
 * Provides:
 *   - connectWifiAP():   first-boot bring-up; tries STA, otherwise opens
 *                        a "MiniSpeedCam" soft-AP for the captive config portal.
 *   - connectWifi():     reconnect helper used by Core 0 when the radar
 *                        wakes the device and STA is dropped.
 *   - disconnectWifi():  cleanly tear down WiFi (used during sleep paths).
 *   - wifiResetButton(): 3-second long-press on WIFI_RESET_PIN clears
 *                        stored credentials and reboots into AP mode.
 *   - sendLocalIP():     announce the device's LAN IP to the cloud so
 *                        the user can reach the config portal remotely.
 *   - sendUpload():       POST a finished run to the cloud — streams the
 *                        captured photo + max speed, or just the speed
 *                        when no photo was captured.
 *
 * The Bearer token and optional TLS root certificate come from config.h
 * (both build-flag overridable); see that file for details.
 */

#include "config.h"
#include <esp_random.h>  // esp_random(): hardware RNG used to mint the per-device identity

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

// True while Core 0 is mid WiFi burst (upload / reconnect / OTA download -- the
// RadarBlankGuard depth is nonzero), a finished run is still queued for upload,
// or the STM32 is being reflashed. taskCore1's power-saver paths must NOT drop
// WiFi while this holds: every burst blanks the radar so speed reads 0, which is
// exactly what would arm the idle-WiFi-off timer -- a self-inflicted teardown
// that kills the in-flight upload and starves the OTA download. See taskCore1.
static inline bool coreNetBusy() {
  return radarBlankDepth > 0
      || stm_flash_busy
      || (uploadQueue != nullptr && uxQueueMessagesWaiting(uploadQueue) > 0);
}

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

// The ESP-IDF Mozilla root-CA bundle (esp_crt_bundle), embedded by the Arduino
// framework when CONFIG_MBEDTLS_CERTIFICATE_BUNDLE is enabled (default). Linked
// symbol; declared here so the GitHub update path can validate against the full
// public trust store rather than one hand-pinned root.
extern const uint8_t rootca_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t rootca_crt_bundle_end[]   asm("_binary_x509_crt_bundle_end");

/**
 * TLS trust policy for the GitHub auto-update path (ota.h).
 *
 * Validates against the full Mozilla root-CA bundle, NOT a single pinned root.
 * GitHub rotates its CA, and a single pin breaks the whole update path on the
 * next rotation with MBEDTLS_ERR_X509_CERT_VERIFY_FAILED (tls -0x2700) -- which
 * is exactly what bricked the auto-update in the field. The bundle covers
 * api.github.com (release metadata) and the *.githubusercontent.com asset hosts
 * (manifest.json + .bin downloads), across the github.com -> githubusercontent
 * redirect HTTPClient follows on one client, and survives future CA rotations.
 * Still REAL certificate validation (not setInsecure): the firmware-update path
 * needs it, because the .bin's trusted MD5 is read from the TLS-fetched manifest.
 */
static void applyTlsPolicyGitHub(WiFiClientSecure& client) {
  client.setCACertBundle(rootca_crt_bundle_start,
                         (size_t)(rootca_crt_bundle_end - rootca_crt_bundle_start));
}

/**
 * Pick the transport for an outbound POST to the configured API base.
 *
 * https:// bases use WiFiClientSecure with the TLS policy above; http:// bases
 * (g_server_secure == false -- e.g. a LAN Home Assistant webhook on :8123) use a
 * plain WiFiClient with no TLS. Both client objects are owned by the caller and
 * must outlive the HTTPClient; this returns the one to hand to https.begin().
 * HTTPClient stores a WiFiClient* and calls connect() through it -- connect() is
 * virtual (Arduino's Client base), so the secure client still performs its TLS
 * handshake when selected. The URL scheme (which sets the port/protocol inside
 * HTTPClient) and g_server_secure are both derived from api_base_url, so they
 * always agree.
 */
static WiFiClient& selectApiClient(WiFiClient& plain, WiFiClientSecure& secure) {
  if (g_server_secure) {
    applyTlsPolicy(secure);
    return secure;
  }
  return plain;
}

// --- Per-device identity ------------------------------------------------------
// The same firmware image flashes to every unit; each device mints its own
// identity at runtime (no per-unit build). loadOrCreateIdentity() generates the
// secrets on first boot ONLY and persists them in NVS; registerDevice() does the
// trust-on-first-use handshake with the cloud.

// 32 hex chars (128 bits) straight from the hardware RNG.
static String makeDeviceToken() {
  char buf[33];
  for (int i = 0; i < 4; i++) snprintf(buf + i * 8, 9, "%08x", (unsigned)esp_random());
  buf[32] = '\0';
  return String(buf);
}

// 6-char human claim code from an unambiguous uppercase alphabet (no 0/O/1/I).
// The alphabet is exactly 32 symbols, so `& 31` selects one without modulo bias.
static String makeClaimCode() {
  static const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";  // 32 symbols
  char buf[7];
  for (int i = 0; i < 6; i++) buf[i] = alphabet[esp_random() & 31];
  buf[6] = '\0';
  return String(buf);
}

// Canonical 12-hex factory MAC, from the same eFuse source as deviceHostname().
static String deviceMacString() {
  uint64_t mac = ESP.getEfuseMac();  // 48-bit factory MAC, byte 0 in the LSB
  char buf[13];
  snprintf(buf, sizeof(buf), "%02X%02X%02X%02X%02X%02X",
           (uint8_t)(mac >> 40), (uint8_t)(mac >> 32), (uint8_t)(mac >> 24),
           (uint8_t)(mac >> 16), (uint8_t)(mac >> 8),  (uint8_t)mac);
  return String(buf);
}

// Load device_token / claim_code / claimed from NVS, generating the two secrets
// on first boot only. Never regenerates an existing token (that would orphan the
// device's cloud record). Call once in setup() AFTER preferences.begin().
void loadOrCreateIdentity() {
  device_token = preferences.getString("dev_token", "");
  if (device_token.length() != 32) {
    device_token = makeDeviceToken();
    preferences.putString("dev_token", device_token);
    Serial.println("[ID] generated new device_token (first boot)");
  }
  claim_code = preferences.getString("claim_code", "");
  if (claim_code.length() != 6) {
    claim_code = makeClaimCode();
    preferences.putString("claim_code", claim_code);
    Serial.println("[ID] generated new claim_code (first boot)");
  }
  device_claimed = preferences.getBool("claimed", false);

  // The claim code is meant to be read off the device, so print it prominently.
  // The token is a secret, so only echo a short masked prefix for support/debug.
  Serial.println("==================================================");
  Serial.printf ("  CLAIM CODE : %s\n", claim_code.c_str());
  Serial.printf ("  device id  : %s   token: %.8s...   %s\n",
                 deviceMacString().c_str(), device_token.c_str(),
                 device_claimed ? "[claimed]" : "[unclaimed]");
  Serial.println("  Enter the claim code in the MiniSpeedCam web app to link this device.");
  Serial.println("==================================================");
}

// POST {mac, device_token, claim_code} to register_device. Trust-on-first-use:
// the first call for a MAC creates the device record; later calls are no-ops and
// NEVER overwrite the stored token. Idempotent, so it is safe on every boot.
// Returns true on any HTTP 200.
bool registerDevice() {
  if (WiFi.getMode() != WIFI_STA || WiFi.status() != WL_CONNECTED) return false;
  RadarBlankGuard _blank;  // blank the radar across the WiFi burst, like every TX
  WiFiClient plainClient;
  WiFiClientSecure secureClient;
  WiFiClient& client = selectApiClient(plainClient, secureClient);  // https.. -> TLS; http.. -> plain (LAN HA)
  HTTPClient https;
  https.setConnectTimeout(5000);
  https.setTimeout(5000);
  https.begin(client, server_register_device);
  https.addHeader("Authorization", "Bearer " API_BEARER_TOKEN);
  https.addHeader("Content-Type", "application/json");
  String body = String("{\"mac\":\"") + deviceMacString() +
                "\",\"device_token\":\"" + device_token +
                "\",\"claim_code\":\"" + claim_code + "\"}";
  int code = https.POST(body);
  https.end();
  Serial.printf("[REG] register_device HTTP %d\n", code);
  return code == 200;
}

// Drive registration from taskCore0: retry with exponential backoff until the
// first 200, then stop for this boot. Call once per Core 0 loop iteration; it is
// a no-op until WiFi STA is connected and again once registration has succeeded.
void serviceRegistration() {
  static bool registered = false;
  static unsigned long nextAttemptAt = 0;     // millis() of the next allowed attempt
  static unsigned long backoffMs = 2000;      // doubles per failure, capped at 5 min
  if (registered) return;
  if (WiFi.getMode() != WIFI_STA || WiFi.status() != WL_CONNECTED) return;
  if ((long)(millis() - nextAttemptAt) < 0) return;  // still backing off

  if (registerDevice()) {
    registered = true;
    g_cloud_ok = true;  // a 200 here proves outbound HTTPS works (OTA health signal)
    Serial.println("[REG] device registered with cloud");
  } else {
    nextAttemptAt = millis() + backoffMs;
    backoffMs = (backoffMs < 300000UL) ? (backoffMs * 2) : 300000UL;  // cap at 5 min
    Serial.printf("[REG] registration failed; next retry in %lu ms\n", backoffMs);
  }
}

/**
 * First-boot WiFi bring-up.
 *
 * Attempts STA mode using credentials previously stored in NVS. If that
 * fails within ~7 seconds, falls back to AP mode so the user can open
 * the config portal at http://192.168.4.1 and enter credentials.
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
    ap_fallback_mode = false;  // STA is up; reconnect-on-radar-motion is safe
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
    // Boot STA failed (or no creds): the soft-AP is now the only way in, so mark
    // fallback mode. connectWifi() and the taskCore1 reconnect trigger both honor
    // this and leave the portal up instead of tearing it down to chase STA.
    ap_fallback_mode = true;

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

  // Boot STA failed and we're serving the config portal over the soft-AP. Don't
  // switch the radio to STA to chase a network that just proved unreachable -- it
  // would drop the portal and strand the device. A reboot retries STA cleanly.
  if (ap_fallback_mode) {
    Serial.println("AP config-fallback mode; keeping the portal up (reboot to retry STA)");
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
  // Never echo the WiFi password to the console -- serial output is trivially
  // captured (USB, boot-log dumps) and this is the user's network secret. Log
  // only whether one is set and its length, which is enough to debug "no
  // password stored" vs "wrong password" without disclosing it.
  Serial.printf("password: %s\n", password.length() ? "[set, redacted]" : "[empty]");
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

  // Keyed by device_token on the server; minted at boot, so this is defensive.
  if (device_token.isEmpty()) {
    Serial.println("device_token not set; skipping IP announce");
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

    WiFiClient plainClient;          // stack-scoped: freed on return, no leak (lives past https.end())
    WiFiClientSecure secureClient;   // ditto
    WiFiClient& client = selectApiClient(plainClient, secureClient);  // https.. -> TLS; http.. -> plain (LAN HA)

    HTTPClient https;

    String recv_token = "Bearer " API_BEARER_TOKEN;  // token from config.h (build-flag overridable)

    // Sending POST request
    https.begin(client, server_local_ip_address);
    https.addHeader("Authorization", recv_token);         // Adding Bearer token as HTTP header
    https.addHeader("Content-Type", "application/json");  // Adding Bearer token as HTTP header

    // Announce the IP plus a lightweight device-health snapshot. This is the
    // one POST that fires regularly while connected, so it doubles as a
    // heartbeat: WiFi signal, heap (incl. the low-water mark for catching slow
    // leaks), uptime, reboot reason/count, and the lifetime rejection counters
    // (corrupt radar replies + proximity-gated distant traffic). All additive
    // fields -- Bubble ignores any it hasn't defined until the workflow maps them.
    httpsRequestData = String("{\"device_token\":\"") + device_token +
                       "\",\"ip_address\":\"" + local_ip_address +
                       "\",\"rssi\":\"" + WiFi.RSSI() +
                       "\",\"free_heap\":\"" + ESP.getFreeHeap() +
                       "\",\"min_free_heap\":\"" + ESP.getMinFreeHeap() +
                       "\",\"uptime_s\":\"" + (millis() / 1000) +
                       "\",\"boot_count\":\"" + diagnosticsBootCount() +
                       "\",\"reset_reason\":\"" + diagnosticsResetReason() +
                       "\",\"esp_fw\":\"" + FW_VERSION +
                       "\",\"stm_fw\":\"" + stm_fw_version +
                       "\",\"reject_speed\":\"" + (unsigned long)g_reject_speed +
                       "\",\"reject_proximity\":\"" + (unsigned long)g_reject_proximity + "\"}";

    Serial.println(httpsRequestData);

    // Send HTTPS POST request
    httpsResponseCode = https.POST(httpsRequestData);
    Serial.print("HTTP Response code: ");
    Serial.println(httpsResponseCode);

    if (httpsResponseCode > 0) {
      payload = https.getString();
      Serial.println(payload);
    }
    if (httpsResponseCode == 200) g_cloud_ok = true;  // OTA health signal: outbound HTTPS works
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
 * carries a photo (req.has_photo / req.photo_buf) the JPEG is base64-streamed
 * on the fly straight from the PSRAM copy via StreamingUploadBody, so the
 * image is never duplicated in heap; otherwise a short speed-only JSON
 * body is sent. The `send_photo` field tells the server which case it is.
 *
 * The caller owns req.photo_buf and must free() it after this returns.
 * speed_actual is already final (the run has ended), so there is
 * no waiting/spinning here.
 */
void sendUpload(const UploadRequest& req) {

  Serial.printf("[UPLOAD] start: %s, heap=%u\n",
                (req.has_photo && req.photo_buf != nullptr) ? "photo" : "speed-only",
                (unsigned)ESP.getFreeHeap());

  // Identity missing (should never happen post-boot): nothing to authenticate
  // the POST with, so skip rather than burning a TLS handshake on a sure failure.
  if (device_token.isEmpty()) {
    Serial.println("[UPLOAD] skipped: device_token not set");
    return;
  }

  // Reject backoff: when the server rejects a capture (empty {} -- device not yet
  // claimed by a user, or the user is over their monthly quota) we cool off for a
  // while so a stream of passes doesn't hammer the API before the device is
  // claimed. Set further down; checked here before spending a connection.
  static unsigned long captureBackoffUntil = 0;
  if ((long)(millis() - captureBackoffUntil) < 0) {
    Serial.printf("[UPLOAD] skipped: backing off after a rejection (%lds left). Claim code: %s\n",
                  (long)(captureBackoffUntil - millis()) / 1000, claim_code.c_str());
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

  // Transport per configured scheme: https.. -> WiFiClientSecure (TLS policy from
  // config.h, pinned cert if set else insecure); http.. -> plain WiFiClient (LAN HA).
  WiFiClient plainClient;          // stack-scoped: freed on return, no leak (lives past https.end())
  WiFiClientSecure secureClient;   // ditto
  WiFiClient& client = selectApiClient(plainClient, secureClient);

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

  // Per-event telemetry, shared by both body shapes (peak_snr and mean_speed
  // are sent as decimals -- the STM32 reports SNR x10, and mean is x10 here).
  // These are ADDITIVE JSON fields: the Bubble capture workflow ignores any
  // parameter it hasn't defined, so sending them is safe before the server is
  // updated -- add matching parameters in the workflow to actually store them.
  String meta = String(",\"direction\":\"") + (int)req.direction +
                "\",\"mag_trend\":\"" + (unsigned)req.mag_trend +
                "\",\"peak_mag\":\"" + (unsigned)req.peak_mag +
                "\",\"peak_snr\":\"" + (req.peak_snr / 10.0f) +
                "\",\"mean_speed\":\"" + (req.mean_speed_x10 / 10.0f) +
                "\",\"frame_count\":\"" + (unsigned)req.frame_count +
                "\",\"duration_ms\":\"" + (unsigned long)req.duration_ms + "\"";

  if (req.has_photo && req.photo_buf != nullptr) {
    // Stream prologue + base64(JPEG) + epilogue without ever holding
    // the whole encoded image in RAM.
    String prologue = "{\"send_photo\":\"true\",\"device_token\":\"";
    prologue += device_token;
    prologue += "\",\"speed_actual\":\"";
    prologue += req.speed_actual;
    prologue += "\"";
    prologue += meta;
    prologue += ",\"photo\":{\"filename\":\"image.jpg\",\"contents\":\"";
    String epilogue = "\"}}";

    StreamingUploadBody body(prologue, req.photo_buf, req.photo_len, epilogue);
    Serial.printf("[UPLOAD] streaming POST, body=%u bytes\n", (unsigned)body.totalSize());
    httpsResponseCode = https.sendRequest("POST", &body, body.totalSize());
  } else {
    // Speed-only event (no photo): short, fully-buffered JSON body.
    String json = "{\"send_photo\":\"false\",\"device_token\":\"";
    json += device_token;
    json += "\",\"speed_actual\":\"";
    json += req.speed_actual;
    json += "\"";
    json += meta;
    json += "}";
    Serial.println("[UPLOAD] speed-only POST");
    httpsResponseCode = https.POST(json);
  }

  Serial.printf("[UPLOAD] HTTP %d %s\n", httpsResponseCode,
                (httpsResponseCode >= 200 && httpsResponseCode < 300) ? "PASS" : "FAIL");
  diagnosticsRecordUpload(httpsResponseCode);  // surface the result on the config portal Status section

  if (httpsResponseCode > 0) {
    payload = https.getString();
    Serial.println(payload);
  }

  // Free resources
  https.end();

  // Interpret the result. Two protocols:
  //   Bubble (minispeedcam.com): {"status":"ok"} == stored; an empty {} == rejected
  //     (device not yet claimed by a user, or the user is over their monthly quota).
  //     A rejection is NOT a hard error -- keep the claim code visible and back off
  //     briefly so we don't hammer the API on every pass while still unclaimed.
  //   Generic endpoint (e.g. a Home Assistant webhook, g_server_is_bubble == false):
  //     there is no claim/quota handshake, so any 2xx means the event was accepted --
  //     an empty 200 is success, not a rejection, and must NOT trip the backoff.
  bool accepted;
  if (g_server_is_bubble) {
    accepted = (httpsResponseCode == 200) && (payload.indexOf("\"ok\"") >= 0);
  } else {
    accepted = (httpsResponseCode >= 200 && httpsResponseCode < 300);
  }
  if (accepted) {
    if (!device_claimed) {
      device_claimed = true;
      preferences.putBool("claimed", true);  // taskCore1 swaps the portal label to "linked"
      Serial.println("[CLAIM] capture accepted -> device is now claimed (hiding claim code)");
    }
  } else if (g_server_is_bubble && httpsResponseCode == 200) {
    // 200 + non-ok body == an explicit Bubble rejection (unclaimed / over quota).
    captureBackoffUntil = millis() + 30000;  // 30s cool-off before the next attempt
    Serial.printf("[UPLOAD] rejected (unclaimed or over quota); backing off 30s. Claim code: %s\n",
                  claim_code.c_str());
  }

  Serial.printf("[UPLOAD] done, heap=%u\n", (unsigned)ESP.getFreeHeap());
}