/**
 * ota.h - ArduinoOTA wrapper for over-the-air firmware updates.
 *
 * Listens on UDP port 3232 (the standard ArduinoOTA port) and accepts
 * pushes from PlatformIO / Arduino IDE / `espota.py` once WiFi is up.
 * Discovery happens via mDNS as "MiniSpeedCam.local"; the matching
 * partition table is default_8MB.csv (see platformio.ini), which gives
 * the bootloader two app slots to flip between.
 *
 * Usage:
 *   - otaBegin() is called from api.cpp immediately after a successful
 *     STA association. It is idempotent: the first call configures
 *     hostname / password / callbacks, every call (re)starts the
 *     listener so OTA survives WiFi disconnect / reconnect cycles.
 *   - otaLoop() is driven from taskCore0 each iteration -- one cheap
 *     UDP poll, nothing happens when no update is incoming.
 *   - g_ota_in_progress goes true while a transfer is mid-flight; the
 *     radar task (Core 1) and the upload task (Core 0) read it to back
 *     off so they don't fight the flash writer for memory / bandwidth.
 *
 * Set the OTA password by defining OTA_PASSWORD at build time, e.g.
 *   build_flags = -DOTA_PASSWORD=\"my-secret\"
 * in platformio.ini. Falls back to a compiled-in default otherwise.
 */
#pragma once

#include <atomic>

#include <Arduino.h>  // for Arduino String

extern std::atomic<bool> g_ota_in_progress;

void otaBegin();
void otaLoop();

// Persist a new OTA password (NVS) and update the live listener.
// Called from the ESPUI Device tab.
void otaSetPassword(const String& password);
