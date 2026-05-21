/**
 * queue.h - LittleFS-backed offline queue for capture events.
 *
 * When the live upload path can't deliver an event (no WiFi, transient
 * server error, rate limit), the captured JPEG plus metadata is
 * written to flash via queueEnqueueEvent(). queueDrain() walks the
 * queue oldest-first and re-runs the upload via
 * apiUploadEventFromMemory(); successfully-delivered events are
 * deleted, the first failure aborts the drain so we don't hammer a
 * broken endpoint.
 *
 * Storage lives in the SPIFFS partition reserved by default_8MB.csv
 * (mounted via LittleFS for power-loss robustness). Files are named
 * sequentially under /queue/.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <Arduino.h>  // Arduino String

/**
 * Mount LittleFS and ensure the /queue/ directory exists. Returns true
 * on success. Call once during setup() AFTER preferences.begin().
 */
bool queueInit();

/**
 * Persist a single event to flash for later retry.
 *
 * @return true if the event was written successfully.
 */
bool queueEnqueueEvent(int speed,
                      long long epoch,
                      const String& camera_id,
                      const uint8_t* jpeg,
                      size_t jpeg_len);

/**
 * Walk the queue oldest-first, attempting to upload each event via
 * apiUploadEventFromMemory(). Removes events on success; aborts at the
 * first failure. Safe to call when WiFi is down (returns immediately).
 */
void queueDrain();

/** Number of events still waiting on disk. */
size_t queuePendingCount();
