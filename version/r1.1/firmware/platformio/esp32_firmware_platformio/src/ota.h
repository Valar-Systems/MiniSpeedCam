/**
 * ota.h - Automatic firmware updates from GitHub Releases.
 *
 * Updates are now fully unattended: there are no web-UI buttons and no Bubble
 * firmware_check. taskCore0 (the networking task) calls otaAutoService() each
 * loop; on a timer it polls the public repo's Releases for the latest tag, reads
 * the attached manifest.json for the per-MCU versions + .bin URLs + MD5s, and
 * applies any STRICTLY NEWER image -- but only when the device is idle (no car
 * pass in progress, no aiming stream, upload queue drained) so a reboot/UART
 * seize never interrupts a capture.
 *
 *   - otaCheckGithub(): GET /releases/latest -> find manifest.json asset; GET it
 *                       -> fill ota_{esp,stm}_{version,url,md5} and the *_available
 *                       flags (strictly-newer test via versionNewer()).
 *   - otaInstallEsp():  stream the ESP .bin into the INACTIVE OTA slot (Update
 *                       lib, MD5-verified), arm the health watchdog, and reboot.
 *                       A power loss mid-download is harmless (old slot boots).
 *   - otaInstallStm():  reflash the STM32 over UART via its ROM bootloader
 *                       (stm32boot.h), MD5-verified before flashing.
 *
 * SAFETY (rollback + health check): a freshly-applied ESP image is on probation.
 * otaHealthService() commits it (and clears the probation) only once the device
 * reaches the cloud (a Bubble or GitHub round-trip) after the reboot; if it
 * can't within OTA_HEALTH_TIMEOUT_MS, or it crash-loops past OTA_HEALTH_MAX_BOOTS,
 * the watchdog sets the boot partition back to the previous (known-good) slot and
 * reboots. The failed version is remembered so it isn't re-applied in a loop.
 *
 * TRUST: the whole path is TLS-authenticated against pinned roots
 * (applyTlsPolicyGitHub), and the binary is MD5-verified before commit as
 * defense-in-depth -- the MD5 itself arrives over the authenticated manifest
 * fetch, so a swapped binary is rejected.
 *
 * NOTE on rollback depth: true bootloader-level rollback (reverting an image
 * that won't even boot far enough to run this code) needs
 * CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE, which this Arduino/pioarduino build does
 * not ship. The app-level watchdog here covers the realistic failure modes
 * (boots-but-can't-connect, crash-after-boot) that survive the pre-flash MD5
 * check. esp_ota_mark_app_valid_cancel_rollback() is still called on commit so
 * the logic is correct if IDF rollback is ever enabled.
 */
#pragma once
#include <stdarg.h>
#include <Update.h>
#include <ArduinoJson.h>
#include <MD5Builder.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

// Format a status line into ota_status (mirrored to the web UI by taskCore1)
// and echo it to the serial console.
static void otaSetStatus(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(ota_status, sizeof(ota_status), fmt, ap);
  va_end(ap);
  Serial.printf("[OTA] %s\n", ota_status);
}

// Parse up to major.minor.patch out of a version string, skipping any leading
// non-digit prefix ("fw-1.0.4" / "v1.0.4" -> 1,0,4). Returns false if no number
// is found at all. Missing components default to 0 ("1.0" -> 1,0,0).
static bool otaParseVer(const String& s, int out[3]) {
  out[0] = out[1] = out[2] = 0;
  int i = 0;
  while (i < (int)s.length() && (s[i] < '0' || s[i] > '9')) i++;
  if (i >= (int)s.length()) return false;
  return sscanf(s.c_str() + i, "%d.%d.%d", &out[0], &out[1], &out[2]) >= 1;
}

// True if `remote` is a strictly newer semantic version than `local`. An
// unparseable local (e.g. the STM "unknown", or a fresh "0.0.0-dev") counts as
// older, so a valid remote always wins; an unparseable/empty remote never wins
// (we won't chase garbage). Equal versions are NOT newer (no needless re-flash).
static bool versionNewer(const String& remote, const String& local) {
  if (remote.isEmpty()) return false;
  int r[3], l[3];
  if (!otaParseVer(remote, r)) return false;
  if (!otaParseVer(local, l)) return true;
  for (int k = 0; k < 3; k++) {
    if (r[k] != l[k]) return r[k] > l[k];
  }
  return false;
}

// Ask GitHub what the latest release is and read its manifest. Fills the
// ota_{esp,stm}_{version,url,md5} globals and the *_available flags. Returns
// true on a clean round-trip (even if nothing newer is offered).
bool otaCheckGithub() {
  if (WiFi.getMode() != WIFI_STA || WiFi.status() != WL_CONNECTED) { otaSetStatus("check: WiFi not connected"); return false; }

  otaSetStatus("checking GitHub for updates...");
  RadarBlankGuard _blank;  // blank the radar across the WiFi burst

  // 1) Latest release -> the manifest.json asset's download URL. The release
  //    JSON is large, so stream-parse only the fields we need with a filter.
  String manifestUrl, tag;
  {
    WiFiClientSecure client;
    applyTlsPolicyGitHub(client);
    HTTPClient https;
    https.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    https.setConnectTimeout(8000);
    https.setTimeout(8000);
    if (!https.begin(client, server_github_latest)) { otaSetStatus("check: bad API URL"); return false; }
    https.addHeader("User-Agent", "MiniSpeedCam-OTA");          // GitHub 403s a missing UA
    https.addHeader("Accept", "application/vnd.github+json");
    int code = https.GET();
    if (code != 200) {
      if (code < 0) {  // negative = TLS/socket failure, not an HTTP status -- surface why
        char tlsbuf[64] = "";
        int tlserr = client.lastError(tlsbuf, sizeof(tlsbuf));
        Serial.printf("[OTA] github connect failed: code=%d tls=-0x%04x (%s)\n", code, -tlserr, tlsbuf);
        otaSetStatus("check: API HTTP %d (tls -0x%04x)", code, -tlserr);
      } else {
        otaSetStatus("check: API HTTP %d", code);
      }
      https.end();
      return false;
    }

    JsonDocument filter;
    filter["tag_name"] = true;
    filter["assets"][0]["name"] = true;
    filter["assets"][0]["browser_download_url"] = true;
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, https.getStream(), DeserializationOption::Filter(filter));
    https.end();
    if (err) { otaSetStatus("check: bad API response"); return false; }

    tag = (const char*)(doc["tag_name"] | "");
    for (JsonObject a : doc["assets"].as<JsonArray>()) {
      if (strcmp((const char*)(a["name"] | ""), "manifest.json") == 0) {
        manifestUrl = (const char*)(a["browser_download_url"] | "");
        break;
      }
    }
    if (manifestUrl.isEmpty()) { otaSetStatus("check: no manifest in %s", tag.c_str()); return false; }
  }

  // 2) manifest.json -> versions + bin URLs + MD5s (small; fully buffered).
  {
    WiFiClientSecure client;
    applyTlsPolicyGitHub(client);
    HTTPClient https;
    https.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);  // github.com -> githubusercontent.com
    https.setConnectTimeout(8000);
    https.setTimeout(8000);
    if (!https.begin(client, manifestUrl)) { otaSetStatus("check: bad manifest URL"); return false; }
    https.addHeader("User-Agent", "MiniSpeedCam-OTA");
    int code = https.GET();
    if (code != 200) { otaSetStatus("check: manifest HTTP %d", code); https.end(); return false; }
    String resp = https.getString();
    https.end();
    Serial.printf("[OTA] manifest <- %s\n", resp.c_str());

    JsonDocument doc;
    if (deserializeJson(doc, resp)) { otaSetStatus("check: bad manifest JSON"); return false; }
    ota_esp_version = (const char*)(doc["esp_version"] | "");
    ota_esp_url     = (const char*)(doc["esp_url"]     | "");
    ota_esp_md5     = (const char*)(doc["esp_md5"]     | "");
    ota_stm_version = (const char*)(doc["stm_version"] | "");
    ota_stm_url     = (const char*)(doc["stm_url"]     | "");
    ota_stm_md5     = (const char*)(doc["stm_md5"]     | "");
  }

  // "Available" = a strictly newer version than ours WITH a URL and a 32-hex MD5.
  ota_esp_available = versionNewer(ota_esp_version, String(FW_VERSION)) && ota_esp_url.length() && ota_esp_md5.length() == 32;
  ota_stm_available = versionNewer(ota_stm_version, stm_fw_version)     && ota_stm_url.length() && ota_stm_md5.length() == 32;

  // A successful GitHub round-trip proves outbound HTTPS works on this image --
  // a valid health signal for the post-update watchdog (otaHealthService()).
  g_cloud_ok = true;

  char espinfo[44], stminfo[44];
  if (ota_esp_available) snprintf(espinfo, sizeof espinfo, "%s->%s", FW_VERSION, ota_esp_version.c_str());
  else                   snprintf(espinfo, sizeof espinfo, "%s ok", FW_VERSION);
  if (ota_stm_available) snprintf(stminfo, sizeof stminfo, "%s->%s", stm_fw_version.c_str(), ota_stm_version.c_str());
  else                   snprintf(stminfo, sizeof stminfo, "%s ok", stm_fw_version.c_str());
  otaSetStatus("ESP %s | STM %s", espinfo, stminfo);
  return true;
}

// Record, in NVS, that the just-flashed ESP image is on probation: which slot we
// were running (to revert to), the boot-count baseline (crash-loop detection),
// and the version (status + bad-version memory). Call right before rebooting.
static void otaApplyArmPending() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  preferences.putUChar("ota_prev", running ? (uint8_t)running->subtype : 0);
  preferences.putULong("ota_bootbase", g_boot_count);
  preferences.putString("ota_pendver", ota_esp_version);
  preferences.putUChar("ota_pend", 1);
  Serial.printf("[OTA] health watchdog armed: prev_subtype=0x%02X bootbase=%lu ver=%s\n",
                running ? running->subtype : 0, (unsigned long)g_boot_count, ota_esp_version.c_str());
}

// Download the ESP32 image into the inactive slot, verify MD5, arm the health
// watchdog, and reboot into it.
void otaInstallEsp() {
  if (!ota_esp_available || ota_esp_url.isEmpty()) { otaSetStatus("ESP: no update"); return; }
  if (WiFi.getMode() != WIFI_STA || WiFi.status() != WL_CONNECTED) { otaSetStatus("ESP: WiFi not connected"); return; }

  otaSetStatus("ESP %s: downloading...", ota_esp_version.c_str());
  RadarBlankGuard _blank;  // held across the whole download; no captures during OTA
  WiFiClientSecure client;
  applyTlsPolicyGitHub(client);
  HTTPClient https;
  https.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);  // github.com -> githubusercontent.com CDN
  https.setConnectTimeout(8000);
  https.setTimeout(8000);
  if (!https.begin(client, ota_esp_url)) { otaSetStatus("ESP: bad URL"); return; }
  https.addHeader("User-Agent", "MiniSpeedCam-OTA");
  int code = https.GET();
  if (code != 200) { otaSetStatus("ESP: download HTTP %d", code); https.end(); return; }
  int len = https.getSize();
  if (len <= 0) { otaSetStatus("ESP: unknown content length"); https.end(); return; }
  if (!Update.begin((size_t)len)) { otaSetStatus("ESP: image too big (%d B)", len); https.end(); return; }
  if (ota_esp_md5.length() == 32) Update.setMD5(ota_esp_md5.c_str());  // verified before commit

  otaSetStatus("ESP %s: writing %d B...", ota_esp_version.c_str(), len);
  WiFiClient* stream = https.getStreamPtr();
  size_t written = Update.writeStream(*stream);
  https.end();
  if (written != (size_t)len) { Update.abort(); otaSetStatus("ESP: short write %u/%d", (unsigned)written, len); return; }
  if (!Update.end(true)) { otaSetStatus("ESP: verify failed (%s)", Update.errorString()); return; }

  otaApplyArmPending();  // probation: otaHealthService() commits or reverts after reboot
  otaSetStatus("ESP %s installed - rebooting (health check armed)", ota_esp_version.c_str());
  delay(1500);  // give taskCore1 a beat to push the final status to the UI
  ESP.restart();
}

// Download the STM32 image, verify it, and flash it over UART via the ROM
// bootloader (stm32boot.h). Holds stm_flash_busy across the flash so taskCore1
// stops polling the radar over the shared UART1. (No rollback for the STM32, so
// the pre-flash MD5 verify is the protection; a known-good STM image can always
// be re-flashed by a later release.)
void otaInstallStm() {
  if (!ota_stm_available || ota_stm_url.isEmpty()) { otaSetStatus("STM: no update"); return; }
  if (WiFi.getMode() != WIFI_STA || WiFi.status() != WL_CONNECTED) { otaSetStatus("STM: WiFi not connected"); return; }

  otaSetStatus("STM %s: downloading...", ota_stm_version.c_str());
  RadarBlankGuard _blank;
  WiFiClientSecure client;
  applyTlsPolicyGitHub(client);
  HTTPClient https;
  https.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  https.setConnectTimeout(8000);
  https.setTimeout(8000);
  if (!https.begin(client, ota_stm_url)) { otaSetStatus("STM: bad URL"); return; }
  https.addHeader("User-Agent", "MiniSpeedCam-OTA");
  int code = https.GET();
  if (code != 200) { otaSetStatus("STM: download HTTP %d", code); https.end(); return; }
  int len = https.getSize();
  if (len <= 0 || len > 256 * 1024) { otaSetStatus("STM: bad size %d", len); https.end(); return; }

  uint8_t* buf = (uint8_t*)ps_malloc(len);
  if (!buf) buf = (uint8_t*)malloc(len);
  if (!buf) { otaSetStatus("STM: out of memory (%d B)", len); https.end(); return; }

  // Read the whole image into the buffer (it's tiny -- the STM32 has 64 KB flash).
  WiFiClient* stream = https.getStreamPtr();
  int got = 0;
  uint32_t t0 = millis();
  while (got < len && (millis() - t0) < 20000) {
    int avail = stream->available();
    if (avail > 0) {
      int r = stream->read(buf + got, (avail < len - got) ? avail : (len - got));
      if (r > 0) { got += r; t0 = millis(); }
    } else if (!client.connected()) {
      break;
    } else {
      delay(1);
    }
  }
  https.end();
  if (got != len) { free(buf); otaSetStatus("STM: short download %d/%d", got, len); return; }

  // Verify MD5 if the manifest provided one (added in chunks; image can exceed the 64 KB cap above).
  if (ota_stm_md5.length() == 32) {
    MD5Builder md5;
    md5.begin();
    for (size_t o = 0; o < (size_t)len; ) {
      uint16_t c = ((size_t)len - o > 16384) ? 16384 : (uint16_t)((size_t)len - o);
      md5.add(buf + o, c);
      o += c;
    }
    md5.calculate();
    if (!md5.toString().equalsIgnoreCase(ota_stm_md5)) {
      free(buf);
      otaSetStatus("STM: MD5 mismatch");
      return;
    }
  }

  otaSetStatus("STM %s: flashing...", ota_stm_version.c_str());
  stm_flash_busy = true;   // Core 0 takes UART1; taskCore1 pauses radar polling
  delay(200);              // let any in-flight get_speed() on taskCore1 finish first
  String err;
  bool ok = stm32FlashImage(buf, (size_t)len, err);
  free(buf);
  if (!ok) {
    stm_flash_busy = false;
    otaSetStatus("STM flash failed: %s", err.c_str());
    return;
  }

  // Confirm by re-reading the version (still under stm_flash_busy, so UART1 is ours).
  delay(200);
  stm_fw_version = get_stm32_version();
  for (int i = 0; i < 3 && stm_fw_version == "unknown"; i++) { delay(50); stm_fw_version = get_stm32_version(); }
  stm_flash_busy = false;  // release UART1 back to taskCore1

  ota_stm_available = false;  // consumed
  otaSetStatus("STM updated -> %s", stm_fw_version.c_str());
}

// Set the boot partition back to the previous (known-good) slot and reboot. The
// failed version is recorded so otaAutoService() won't re-apply it in a loop.
static void otaRevertPending(const char* reason) {
  uint8_t prev = preferences.getUChar("ota_prev", 0);
  String ver = preferences.getString("ota_pendver", "");

  // Clear probation + remember the bad version BEFORE rebooting, so we never
  // loop on the revert and never re-download the same broken image.
  if (ver.length()) preferences.putString("ota_esp_bad", ver);
  preferences.remove("ota_pend");
  preferences.remove("ota_bootbase");
  preferences.remove("ota_prev");
  preferences.remove("ota_pendver");

  const esp_partition_t* p = (prev == ESP_PARTITION_SUBTYPE_APP_OTA_0 || prev == ESP_PARTITION_SUBTYPE_APP_OTA_1)
      ? esp_partition_find_first(ESP_PARTITION_TYPE_APP, (esp_partition_subtype_t)prev, NULL)
      : nullptr;
  if (p && esp_ota_set_boot_partition(p) == ESP_OK) {
    otaSetStatus("ESP %s unhealthy (%s) - reverting", ver.c_str(), reason);
    Serial.printf("[OTA] reverting to subtype 0x%02X and rebooting\n", prev);
    delay(1500);
    ESP.restart();
  } else {
    // Can't find/set the previous slot -- staying put beats bricking. Probation
    // is already cleared, so we just keep running this image.
    otaSetStatus("ESP revert FAILED (%s) - staying", reason);
  }
}

// Fast crash-loop guard. Call once in setup() (after diagnosticsLogBoot(), so
// g_boot_count is current). If a probationary image has rebooted more than
// OTA_HEALTH_MAX_BOOTS times without ever committing, revert immediately --
// before WiFi even comes up -- so a boot-looping image is caught quickly.
void otaHealthBootCheck() {
  if (!preferences.getUChar("ota_pend", 0)) return;
  uint32_t base = preferences.getULong("ota_bootbase", g_boot_count);
  uint32_t attempts = (g_boot_count >= base) ? (g_boot_count - base) : 1;
  Serial.printf("[OTA] pending image on probation: attempt %lu/%d\n",
                (unsigned long)attempts, OTA_HEALTH_MAX_BOOTS);
  if (attempts > (uint32_t)OTA_HEALTH_MAX_BOOTS) {
    otaRevertPending("crash-loop");  // reboots into the previous slot
  }
}

// Commit-or-revert service for a probationary ESP image. Call each taskCore0
// loop. Commits once the device reaches the cloud (proves the new image's
// networking works); reverts if it can't within OTA_HEALTH_TIMEOUT_MS.
void otaHealthService() {
  static int done = 0;  // 0 = still evaluating this boot, 1 = settled
  if (done) return;
  if (!preferences.getUChar("ota_pend", 0)) { done = 1; return; }

  String ver = preferences.getString("ota_pendver", "");

  if (g_cloud_ok) {
    // Healthy: the new image reached the cloud. Commit it.
    esp_ota_mark_app_valid_cancel_rollback();  // no-op unless IDF rollback is enabled; correct if it is
    preferences.remove("ota_pend");
    preferences.remove("ota_bootbase");
    preferences.remove("ota_prev");
    preferences.remove("ota_pendver");
    preferences.remove("ota_esp_bad");  // this version proved good; clear any stale block
    otaSetStatus("ESP %s confirmed healthy", ver.c_str());
    done = 1;
    return;
  }

  if (millis() > OTA_HEALTH_TIMEOUT_MS) {
    otaRevertPending("no cloud within health window");  // reboots
    done = 1;  // unreached if the revert reboots, but safe if it couldn't
    return;
  }
  // else: still within the window, keep waiting for a cloud round-trip.
}

// Run the automatic update cycle from taskCore0. No-op until WiFi STA is up and
// the post-boot delay has elapsed; then checks GitHub on a timer and applies any
// strictly-newer image WHILE IDLE (so a reboot/UART seize never interrupts a
// capture). STM is applied first (no reboot), then ESP (which reboots on success).
void otaAutoService() {
  static unsigned long nextCheck = OTA_BOOT_CHECK_DELAY_MS;  // first check after the boot delay

  if (stm_flash_busy) return;
  if (WiFi.getMode() != WIFI_STA || WiFi.status() != WL_CONNECTED) return;
  if ((long)(millis() - nextCheck) < 0) return;

  // Only act when idle. Don't advance the timer if busy, so we retry promptly
  // once the current pass/stream/upload finishes (passes last only seconds).
  bool idle = !g_run_active && !stream_active &&
              (uploadQueue == nullptr || uxQueueMessagesWaiting(uploadQueue) == 0);
  if (!idle) return;

  nextCheck = millis() + OTA_CHECK_INTERVAL_MS;  // schedule the next check regardless of outcome

  if (!otaCheckGithub()) return;

  if (ota_stm_available) otaInstallStm();

  if (ota_esp_available) {
    if (ota_esp_version == preferences.getString("ota_esp_bad", "")) {
      otaSetStatus("ESP %s previously failed health; skipping", ota_esp_version.c_str());
    } else {
      otaInstallEsp();  // reboots on success (never returns)
    }
  }
}
