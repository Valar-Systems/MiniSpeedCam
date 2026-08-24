/**
 * last_photo.h - Retain the most recent capture's JPEG in PSRAM so the config
 * portal can show the last speed photo (GET /api/last.jpg) plus its metadata
 * (the "Last capture" card).
 *
 * The cloud upload frees its JPEG copy as soon as the run is POSTed (see
 * taskCore0 in main.cpp / sendUpload in api.h), so nothing normally survives
 * for the UI to display. This module keeps ONE extra JPEG copy alive in PSRAM.
 * A UXGA/quality-20 frame is a few hundred KB; the board's 8 MB OPI PSRAM
 * already holds the camera driver's two framebuffers, so one more retained
 * frame is a small, bounded cost. Internal SRAM is never touched.
 *
 * Concurrency -- the tricky part. The radar task (Core 1) stages a new photo at
 * run-end via lastPhotoStage(); the port-80 httpd task serves it via
 * lastPhotoBorrow()/lastPhotoRelease(). esp_http_server runs its URI handlers
 * in a single task, so the reader never overlaps itself: the only race is one
 * writer vs. one reader. A tiny portMUX critical section guards the pointer
 * swap (never malloc/free inside it). The slow WiFi send runs with NO lock
 * held. If the writer replaces the buffer while the reader is mid-send, the old
 * buffer is handed to the reader to free on release (lastPhotoRelease) rather
 * than freed under its feet -- so a fast car during a slow page load can never
 * cause a use-after-free.
 *
 * Include order (main.cpp): after variables.h/events.h and before
 * config_portal.h (whose /api/last.jpg handler and /api/state builder call in).
 */
#pragma once

#include <Arduino.h>
#include <esp_timer.h>   // esp_timer_get_time(): 64-bit us clock for the pass age
#include <string.h>

static portMUX_TYPE g_lp_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t* g_lp_buf      = nullptr;   // current retained JPEG (PSRAM); nullptr = none held yet
static size_t   g_lp_len      = 0;
static uint8_t* g_lp_inflight = nullptr;   // buffer the reader is currently sending (borrowed, do not free)
static uint8_t* g_lp_reap     = nullptr;   // buffer the writer retired while borrowed; the reader frees it on release
// Metadata of the pass whose photo we hold (g_lp_seq matches an /api/events seq):
static uint32_t g_lp_seq      = 0;         // 0 = no photo held
static int      g_lp_speed    = 0;         // run max speed, in the unit given by g_lp_kph
static uint8_t  g_lp_kph      = 0;         // unit AT CAPTURE (1 = kph, 0 = mph)
static uint8_t  g_lp_dir      = 0;         // 0 = unknown, 1 = approaching, 2 = receding
static int64_t  g_lp_at_us    = 0;         // esp_timer_get_time() at capture (64-bit; age never wraps)

// Radar task (Core 1): stage a copy of `buf` (len bytes) as the last capture.
// Allocates a fresh PSRAM copy so ownership is independent of the upload buffer
// that Core 0 frees after POSTing. On allocation failure the previous photo is
// kept unchanged (better a stale image than none). `seq` ties the photo to its
// /api/events pass so the portal can tell whether the newest pass has an image.
static void lastPhotoStage(const uint8_t* buf, size_t len, uint32_t seq,
                           int speed, bool kph, uint8_t dir) {
  if (buf == nullptr || len == 0) return;
  uint8_t* copy = (uint8_t*)ps_malloc(len);
  if (copy == nullptr) return;             // keep the existing retained photo rather than dropping it
  memcpy(copy, buf, len);
  const int64_t now_us = esp_timer_get_time();  // read the clock outside the critical section

  uint8_t* to_free = nullptr;
  portENTER_CRITICAL(&g_lp_mux);
  uint8_t* old = g_lp_buf;
  g_lp_buf   = copy;
  g_lp_len   = len;
  g_lp_seq   = seq;
  g_lp_speed = speed;
  g_lp_kph   = kph ? 1 : 0;
  g_lp_dir   = dir;
  g_lp_at_us = now_us;
  if (old != nullptr && old == g_lp_inflight) {
    g_lp_reap = old;   // reader is sending `old`; it frees it on release. (Only ever one borrow at a
                       // time -- single httpd task -- so g_lp_reap holds at most one pending buffer.)
  } else {
    to_free = old;     // nobody is reading `old`; free it once we leave the critical section
  }
  portEXIT_CRITICAL(&g_lp_mux);
  if (to_free) free(to_free);
}

// httpd task: borrow the retained JPEG for sending. Returns nullptr (and sets
// *len = 0) when no photo is held. MUST be paired with lastPhotoRelease(): while
// borrowed, the writer will not free this buffer.
static const uint8_t* lastPhotoBorrow(size_t* len) {
  portENTER_CRITICAL(&g_lp_mux);
  const uint8_t* b = g_lp_buf;
  if (b == nullptr) {
    portEXIT_CRITICAL(&g_lp_mux);
    if (len) *len = 0;
    return nullptr;
  }
  if (len) *len = g_lp_len;
  g_lp_inflight = g_lp_buf;
  portEXIT_CRITICAL(&g_lp_mux);
  return b;
}

// httpd task: end a borrow started by lastPhotoBorrow(). Frees any buffer the
// writer retired while the send was in flight.
static void lastPhotoRelease(void) {
  uint8_t* to_free = nullptr;
  portENTER_CRITICAL(&g_lp_mux);
  g_lp_inflight = nullptr;
  if (g_lp_reap != nullptr) { to_free = g_lp_reap; g_lp_reap = nullptr; }
  portEXIT_CRITICAL(&g_lp_mux);
  if (to_free) free(to_free);
}

// httpd task: snapshot the retained photo's metadata for the "Last capture"
// card. Returns false when no photo is held. ageSec is seconds since the pass.
static bool lastPhotoMeta(uint32_t* seq, int* speed, bool* kph, uint8_t* dir, uint32_t* ageSec) {
  const int64_t now_us = esp_timer_get_time();
  portENTER_CRITICAL(&g_lp_mux);
  if (g_lp_seq == 0) { portEXIT_CRITICAL(&g_lp_mux); return false; }
  const uint32_t s = g_lp_seq;
  const int      sp = g_lp_speed;
  const uint8_t  k = g_lp_kph;
  const uint8_t  d = g_lp_dir;
  const int64_t  at = g_lp_at_us;
  portEXIT_CRITICAL(&g_lp_mux);
  if (seq)    *seq = s;
  if (speed)  *speed = sp;
  if (kph)    *kph = (bool)k;
  if (dir)    *dir = d;
  if (ageSec) *ageSec = (uint32_t)((now_us - at) / 1000000LL);
  return true;
}
