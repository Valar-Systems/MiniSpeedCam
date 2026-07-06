/**
 * events.h - Recent-detection ring buffer + GET /api/events payload builder.
 *
 * The cloud upload (sendUpload) is fire-and-forget: once a run's max speed is POSTed to
 * minispeedcam.com the device keeps no queryable history. This module adds a tiny in-RAM
 * ring of the most recent passes so a LAN companion display -- e.g. the Blipscope
 * "Speedscope" edition -- can show recent speeds without touching the cloud or a key.
 *
 * It is deliberately minimal and independent of the config page: eventsRecord() is called
 * from the radar task (Core 1) at run-end, and eventsBuildJson() is called from the httpd
 * task that serves GET /api/events (registered in config_portal.h). Those two run on
 * different cores, so a short portMUX critical section guards the shared indices while the
 * ring is snapshotted; the JSON is then built outside the lock (it allocates).
 *
 * This device has no NTP/RTC (see diagnostics.h -- only millis() is available), so each
 * event carries `ageSec` (seconds since the pass, from millis()) rather than a wall-clock
 * epoch; a companion with its own synced clock turns that into an absolute time. Speeds are
 * whole numbers in the device's configured unit -- the top-level `kph` flag says which.
 *
 * Include order (main.cpp): after variables.h (uses `is_kph` + millis()) and before
 * config_portal.h (which calls eventsBuildJson from its /api/events handler).
 */
#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <string.h>

struct SpeedEventRec {
  int      speed;   // run max speed, device units (mph or kph per is_kph)
  uint8_t  dir;     // 0 = unknown, 1 = approaching (front), 2 = receding (rear)
  uint16_t mag;     // strongest FFT peak magnitude of the run (proximity/confidence proxy)
  uint32_t at_ms;   // millis() at capture (for a device-relative age)
};

static const int SPEED_EVENTS_CAP = 32;            // last N passes retained
static SpeedEventRec g_speed_events[SPEED_EVENTS_CAP];
// head/count aren't volatile: every access is inside the g_speed_events_mux critical section below,
// which provides the cross-core ordering (a spinlock + interrupt disable), so plain ints are correct.
static int           g_speed_events_head  = 0;     // next slot to write
static int           g_speed_events_count = 0;     // valid entries (<= CAP)
static portMUX_TYPE  g_speed_events_mux   = portMUX_INITIALIZER_UNLOCKED;

// Radar task (Core 1): record one finished run. Called for every pass (speed-only or with
// a photo), so the companion sees the same events the cloud does. Cheap + non-blocking.
static void eventsRecord(int speed, uint8_t dir, uint16_t mag) {
  portENTER_CRITICAL(&g_speed_events_mux);
  SpeedEventRec& e = g_speed_events[g_speed_events_head];
  e.speed = speed;
  e.dir   = dir;
  e.mag   = mag;
  e.at_ms = millis();
  g_speed_events_head = (g_speed_events_head + 1) % SPEED_EVENTS_CAP;
  if (g_speed_events_count < SPEED_EVENTS_CAP) g_speed_events_count++;
  portEXIT_CRITICAL(&g_speed_events_mux);
}

// httpd task: serialize the ring newest-first into `out`:
//   {"kph":false,"count":3,"events":[{"speed":31,"ageSec":4,"mag":1200,"dir":1}, ...]}
static void eventsBuildJson(String& out) {
  // Snapshot the whole physical ring + indices under the lock (a fixed 32-entry copy, a few
  // hundred bytes on the stack), then release it before building/allocating the JSON.
  SpeedEventRec snap[SPEED_EVENTS_CAP];
  int count, head;
  const uint32_t now = millis();
  portENTER_CRITICAL(&g_speed_events_mux);
  count = g_speed_events_count;
  head  = g_speed_events_head;
  memcpy(snap, g_speed_events, sizeof(snap));
  portEXIT_CRITICAL(&g_speed_events_mux);

  JsonDocument doc;
  doc["kph"]   = is_kph;   // defined in variables.h (same translation unit)
  doc["count"] = count;
  JsonArray arr = doc["events"].to<JsonArray>();
  for (int i = 0; i < count; i++) {
    // newest first: step back from the write head, wrapping into the valid region.
    const int idx = ((head - 1 - i) % SPEED_EVENTS_CAP + SPEED_EVENTS_CAP) % SPEED_EVENTS_CAP;
    const SpeedEventRec& e = snap[idx];
    JsonObject o = arr.add<JsonObject>();
    o["speed"]  = e.speed;
    o["ageSec"] = (int)((now - e.at_ms) / 1000UL);
    o["mag"]    = e.mag;
    o["dir"]    = e.dir;
  }

  serializeJson(doc, out);
}
