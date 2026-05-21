#include "ota.h"

#include <Arduino.h>
#include <ArduinoOTA.h>
#include <esp_camera.h>

#include "api.h"  // for HOSTNAME

std::atomic<bool> g_ota_in_progress{false};

// Default OTA password. Override at build time via
//   build_flags = -DOTA_PASSWORD=\"your-password\"
// in platformio.ini.
#ifndef OTA_PASSWORD
#define OTA_PASSWORD "minispeedcam"
#endif

static void onStart() {
  g_ota_in_progress.store(true);
  const char* what = (ArduinoOTA.getCommand() == U_FLASH) ? "firmware" : "filesystem";
  Serial.printf("OTA start (%s)\n", what);
  // Releasing the camera driver frees its DMA descriptors and PSRAM
  // framebuffer so they don't compete with the OTA flash writer. The
  // device reboots into the new image when the transfer completes, so
  // a full re-init happens naturally on the next setup() pass.
  esp_camera_deinit();
}

static void onEnd() {
  Serial.println("\nOTA complete; rebooting into the new image");
  // The library issues ESP.restart() after this callback returns;
  // clearing the flag here is mostly cosmetic.
  g_ota_in_progress.store(false);
}

static void onProgress(unsigned int progress, unsigned int total) {
  if (total == 0) return;
  // Log once per decile so we don't flood Serial during a 1.5 MB upload.
  static unsigned int last_decile = 0xFFFFFFFFu;
  unsigned int decile = (progress * 10u) / total;
  if (decile != last_decile) {
    last_decile = decile;
    Serial.printf("OTA progress: %u%%\n", decile * 10u);
  }
}

static void onError(ota_error_t err) {
  g_ota_in_progress.store(false);
  Serial.printf("OTA error[%u]: ", err);
  switch (err) {
    case OTA_AUTH_ERROR:    Serial.println("auth failed"); break;
    case OTA_BEGIN_ERROR:   Serial.println("begin failed"); break;
    case OTA_CONNECT_ERROR: Serial.println("connect failed"); break;
    case OTA_RECEIVE_ERROR: Serial.println("receive failed"); break;
    case OTA_END_ERROR:     Serial.println("end failed"); break;
    default:                Serial.println("unknown"); break;
  }
}

void otaBegin() {
  static bool configured = false;
  if (!configured) {
    ArduinoOTA.setHostname(HOSTNAME);
    ArduinoOTA.setPassword(OTA_PASSWORD);
    ArduinoOTA.onStart(onStart);
    ArduinoOTA.onEnd(onEnd);
    ArduinoOTA.onProgress(onProgress);
    ArduinoOTA.onError(onError);
    configured = true;
  }
  ArduinoOTA.begin();
  Serial.printf("OTA listening as %s.local:3232\n", HOSTNAME);
}

void otaLoop() {
  ArduinoOTA.handle();
}
