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
 * Payload contract (all fields additive/back-compatible):
 *   {
 *     "kph": false,          // the device's CURRENT unit setting (is_kph)
 *     "count": 3,            // number of events in the ring (<= SPEED_EVENTS_CAP)
 *     "latestSeq": 42,       // seq of the newest event (0 = none); poll this to spot new/missed passes
 *     "events": [            // newest first
 *       {"seq":42,"speed":31,"kph":false,"ageSec":4,"mag":1200,"dir":1}, ...
 *     ]
 *   }
 * Three properties make the feed safe for a stateful companion:
 *   - Per-record "kph": the unit is captured AT RECORD TIME, so changing the device's
 *     units setting later never relabels earlier passes (the top-level "kph" is only the
 *     current setting). Each record's speed is authoritative under its own "kph".
 *   - Monotonic "seq": a unique, ever-increasing id per pass. A companion polling the feed
 *     uses it to dedupe and to detect passes it missed when the 32-entry ring wrapped
 *     between polls (a gap in seq, or latestSeq jumping by more than it received).
 *   - 64-bit age clock: this device has no NTP/RTC (see diagnostics.h), so each event
 *     carries "ageSec" (seconds since the pass) rather than a wall-clock epoch. The source
 *     is esp_timer_get_time() (int64 microseconds since boot), NOT millis() -- millis()
 *     wraps every 49.7 days, which would corrupt the age of any entry older than that on a
 *     long-lived roadside device. A companion with its own synced clock turns ageSec into
 *     an absolute time. Speeds are whole numbers in each record's unit.
 *
 * Include order (main.cpp): after variables.h (uses `is_kph`) and before config_portal.h
 * (which calls eventsBuildJson from its /api/events handler).
 */
#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_timer.h>   // esp_timer_get_time(): 64-bit us clock (millis() wraps at 49.7d)
#include <string.h>

struct SpeedEventRec {
  int      speed;   // run max speed, in the unit given by `kph` below
  uint8_t  dir;     // 0 = unknown, 1 = approaching (front), 2 = receding (rear)
  uint8_t  kph;     // unit of `speed` AT RECORD TIME (1 = kph, 0 = mph)
  uint16_t mag;     // strongest FFT peak magnitude of the run (proximity/confidence proxy)
  uint32_t seq;     // monotonic id, unique per pass (companion dedupe / gap detection)
  int64_t  at_us;   // esp_timer_get_time() at capture; 64-bit so age never wraps
};

static const int SPEED_EVENTS_CAP = 32;            // last N passes retained
static SpeedEventRec g_speed_events[SPEED_EVENTS_CAP];
// head/count/seq aren't volatile: every access is inside the g_speed_events_mux critical section
// below, which provides the cross-core ordering (a spinlock + interrupt disable), so plain ints
// are correct.
static int           g_speed_events_head  = 0;     // next slot to write
static int           g_speed_events_count = 0;     // valid entries (<= CAP)
static uint32_t      g_speed_events_seq   = 0;     // last assigned seq (0 = no events yet)
static portMUX_TYPE  g_speed_events_mux   = portMUX_INITIALIZER_UNLOCKED;

// Radar task (Core 1): record one finished run. Called for every pass (speed-only or with
// a photo), so the companion sees the same events the cloud does. Cheap + non-blocking.
// Returns the monotonic seq assigned to this pass, so the caller can tie an attached photo
// (last_photo.h) to the same event the /api/events feed and the "Last capture" card share.
static uint32_t eventsRecord(int speed, uint8_t dir, uint16_t mag) {
  const int64_t now_us = esp_timer_get_time();   // read the clock outside the lock (no allocation)
  portENTER_CRITICAL(&g_speed_events_mux);
  SpeedEventRec& e = g_speed_events[g_speed_events_head];
  e.speed = speed;
  e.dir   = dir;
  e.kph   = is_kph ? 1 : 0;        // capture the unit now; a later units change won't relabel this pass
  e.mag   = mag;
  e.seq   = ++g_speed_events_seq;   // 1-based monotonic id (first pass is seq 1)
  e.at_us = now_us;
  g_speed_events_head = (g_speed_events_head + 1) % SPEED_EVENTS_CAP;
  if (g_speed_events_count < SPEED_EVENTS_CAP) g_speed_events_count++;
  const uint32_t seq = e.seq;
  portEXIT_CRITICAL(&g_speed_events_mux);
  return seq;
}

// httpd task: fetch the newest pass (speed/dir/unit/seq + age since it happened) for the
// config portal's "Last capture" card. Returns false if no pass has been recorded yet.
static bool eventsLatest(int* speed, uint8_t* dir, bool* kph, uint32_t* seq, uint32_t* ageSec) {
  const int64_t now_us = esp_timer_get_time();
  portENTER_CRITICAL(&g_speed_events_mux);
  if (g_speed_events_count == 0) { portEXIT_CRITICAL(&g_speed_events_mux); return false; }
  const int idx = ((g_speed_events_head - 1) % SPEED_EVENTS_CAP + SPEED_EVENTS_CAP) % SPEED_EVENTS_CAP;
  const SpeedEventRec e = g_speed_events[idx];   // copy out under the lock
  portEXIT_CRITICAL(&g_speed_events_mux);
  if (speed)  *speed = e.speed;
  if (dir)    *dir = e.dir;
  if (kph)    *kph = (bool)e.kph;
  if (seq)    *seq = e.seq;
  if (ageSec) *ageSec = (uint32_t)((now_us - e.at_us) / 1000000LL);
  return true;
}

// httpd task: serialize the ring newest-first into `out` (see the payload contract above).
static void eventsBuildJson(String& out) {
  // Snapshot the whole physical ring + indices under the lock (a fixed 32-entry copy, a few
  // hundred bytes on the stack), then release it before building/allocating the JSON.
  SpeedEventRec snap[SPEED_EVENTS_CAP];
  int count, head;
  uint32_t latest_seq;
  const int64_t now_us = esp_timer_get_time();
  portENTER_CRITICAL(&g_speed_events_mux);
  count      = g_speed_events_count;
  head       = g_speed_events_head;
  latest_seq = g_speed_events_seq;
  memcpy(snap, g_speed_events, sizeof(snap));
  portEXIT_CRITICAL(&g_speed_events_mux);

  JsonDocument doc;
  doc["kph"]       = is_kph;      // current device setting; per-record "kph" is authoritative per pass
  doc["count"]     = count;
  doc["latestSeq"] = latest_seq;  // 0 = no events yet
  JsonArray arr = doc["events"].to<JsonArray>();
  for (int i = 0; i < count; i++) {
    // newest first: step back from the write head, wrapping into the valid region.
    const int idx = ((head - 1 - i) % SPEED_EVENTS_CAP + SPEED_EVENTS_CAP) % SPEED_EVENTS_CAP;
    const SpeedEventRec& e = snap[idx];
    JsonObject o = arr.add<JsonObject>();
    o["seq"]    = e.seq;
    o["speed"]  = e.speed;
    o["kph"]    = (bool)e.kph;   // unit of THIS record (may differ from the top-level current setting)
    o["ageSec"] = (long)((now_us - e.at_us) / 1000000LL);  // 64-bit math: never wraps
    o["mag"]    = e.mag;
    o["dir"]    = e.dir;
  }

  serializeJson(doc, out);
}
