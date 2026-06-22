/**
 * ota.h - Manual-approve firmware updates pulled from the cloud.
 *
 * Triggered from the web UI: an ESPUI button callback sets ota_request (an
 * atomic int) and returns immediately; taskCore0 (the networking task) calls
 * otaServiceCore0() each loop and does the slow work there, so the AsyncTCP /
 * ESPUI event loop is never blocked. Progress is written to ota_status, which
 * taskCore1 mirrors to the web UI (the one task allowed to touch ESPUI).
 *
 * Actions (all on Core 0):
 *   - otaCheck():      POST current versions to server_firmware_check; parse the
 *                      latest available esp/stm versions + .bin URLs + MD5s.
 *   - otaInstallEsp(): stream the ESP .bin into the INACTIVE OTA slot (Update
 *                      lib, MD5-verified) and reboot into it. A power loss mid-
 *                      download is harmless -- the old slot still boots.
 *   - otaInstallStm(): reflash the STM32 over UART via its ROM bootloader
 *                      (implemented in Phase 3; a stub for now).
 *
 * The .bin URLs come back from the (Bearer-authed) check endpoint, so the raw
 * download is fetched without an auth header -- the URL itself is the capability
 * (it may be a short-lived/presigned link).
 */
#pragma once
#include <stdarg.h>
#include <Update.h>
#include <ArduinoJson.h>
#include <MD5Builder.h>

// Format a status line into ota_status (mirrored to the web UI by taskCore1)
// and echo it to the serial console.
static void otaSetStatus(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(ota_status, sizeof(ota_status), fmt, ap);
  va_end(ap);
  Serial.printf("[OTA] %s\n", ota_status);
}

// Ask the cloud what the latest firmware is for this device's two MCUs.
void otaCheck() {
  if (camera_id == "NOT_SET" || camera_id.isEmpty()) { otaSetStatus("check: set Camera ID first"); return; }
  if (WiFi.getMode() != WIFI_STA || WiFi.status() != WL_CONNECTED) { otaSetStatus("check: WiFi not connected"); return; }

  otaSetStatus("checking for updates...");
  RadarBlankGuard _blank;  // blank the radar across the WiFi burst
  WiFiClientSecure client;
  applyTlsPolicy(client);
  HTTPClient https;
  https.begin(client, server_firmware_check);
  https.addHeader("Authorization", "Bearer " API_BEARER_TOKEN);
  https.addHeader("Content-Type", "application/json");
  String body = String("{\"camera\":\"") + camera_id +
                "\",\"esp_fw\":\"" + FW_VERSION +
                "\",\"stm_fw\":\"" + stm_fw_version + "\"}";
  int code = https.POST(body);
  if (code != 200) { otaSetStatus("check failed: HTTP %d", code); https.end(); return; }
  String resp = https.getString();
  https.end();

  JsonDocument doc;
  if (deserializeJson(doc, resp)) { otaSetStatus("check: bad response"); return; }
  // Bubble wraps workflow return values under "response"; tolerate a flat object.
  JsonObject root = doc.as<JsonObject>();
  JsonObject r = root["response"].is<JsonObject>() ? root["response"].as<JsonObject>() : root;

  ota_esp_version = (const char*)(r["esp_version"] | "");
  ota_esp_url     = (const char*)(r["esp_url"]     | "");
  ota_esp_md5     = (const char*)(r["esp_md5"]     | "");
  ota_stm_version = (const char*)(r["stm_version"] | "");
  ota_stm_url     = (const char*)(r["stm_url"]     | "");
  ota_stm_md5     = (const char*)(r["stm_md5"]     | "");

  // "Available" = the cloud names a non-empty version different from ours and
  // gives a URL to fetch. Any difference counts (manual approve decides), so a
  // deliberate downgrade is allowed too.
  ota_esp_available = ota_esp_version.length() && ota_esp_url.length() && ota_esp_version != String(FW_VERSION);
  ota_stm_available = ota_stm_version.length() && ota_stm_url.length() && ota_stm_version != stm_fw_version;

  otaSetStatus("ESP %s%s | STM %s%s",
    ota_esp_available ? ota_esp_version.c_str() : FW_VERSION,
    ota_esp_available ? " UPDATE" : " ok",
    ota_stm_available ? ota_stm_version.c_str() : stm_fw_version.c_str(),
    ota_stm_available ? " UPDATE" : " ok");
}

// Download the ESP32 image into the inactive slot, verify, and reboot into it.
void otaInstallEsp() {
  if (!ota_esp_available || ota_esp_url.isEmpty()) { otaSetStatus("ESP: no update (run Check first)"); return; }
  if (WiFi.getMode() != WIFI_STA || WiFi.status() != WL_CONNECTED) { otaSetStatus("ESP: WiFi not connected"); return; }

  otaSetStatus("ESP %s: downloading...", ota_esp_version.c_str());
  RadarBlankGuard _blank;  // held across the whole download; no captures during OTA
  WiFiClientSecure client;
  applyTlsPolicy(client);
  HTTPClient https;
  https.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);  // Bubble/S3 file URLs redirect to a CDN
  https.setConnectTimeout(8000);
  https.setTimeout(8000);
  if (!https.begin(client, ota_esp_url)) { otaSetStatus("ESP: bad URL"); return; }
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

  otaSetStatus("ESP %s installed - rebooting", ota_esp_version.c_str());
  delay(1500);  // give taskCore1 a beat to push the final status to the UI
  ESP.restart();
}

// Download the STM32 image, verify it, and flash it over UART via the ROM
// bootloader (stm32boot.h). Holds stm_flash_busy across the flash so taskCore1
// stops polling the radar over the shared UART1.
void otaInstallStm() {
  if (!ota_stm_available || ota_stm_url.isEmpty()) { otaSetStatus("STM: no update (run Check first)"); return; }
  if (WiFi.getMode() != WIFI_STA || WiFi.status() != WL_CONNECTED) { otaSetStatus("STM: WiFi not connected"); return; }

  otaSetStatus("STM %s: downloading...", ota_stm_version.c_str());
  RadarBlankGuard _blank;
  WiFiClientSecure client;
  applyTlsPolicy(client);
  HTTPClient https;
  https.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  https.setConnectTimeout(8000);
  https.setTimeout(8000);
  if (!https.begin(client, ota_stm_url)) { otaSetStatus("STM: bad URL"); return; }
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

  // Verify MD5 if the server provided one (added in chunks; image can exceed 64 KB cap above).
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

// Run any web-UI-requested OTA action. Call from taskCore0 each loop.
void otaServiceCore0() {
  int req = ota_request;
  if (req == OTA_NONE) return;
  ota_request = OTA_NONE;
  switch (req) {
    case OTA_CHECK:       otaCheck();      break;
    case OTA_INSTALL_ESP: otaInstallEsp(); break;
    case OTA_INSTALL_STM: otaInstallStm(); break;
  }
}
