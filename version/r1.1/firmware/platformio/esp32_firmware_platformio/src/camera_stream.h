/**
 * camera_stream.h - Public API for the temporary MJPEG "aiming" video stream.
 *
 * The implementation (camera_stream.cpp) runs a lightweight esp_http_server and
 * lives in its own translation unit. With ESPUI/ESPAsyncWebServer gone this is
 * no longer forced by the old HTTP_* enum clash (the config portal also uses
 * esp_http_server now), but the split is kept on purpose: this TU stays free of
 * variables.h, so it can't multiply-define the shared globals -- it reaches the
 * two desired-state flags by extern instead.
 *
 * NOTE: do not name the implementation header "stream.h" -- on a case-insensitive
 * filesystem that shadows the Arduino core's own Stream.h (pulled in by Udp.h),
 * which breaks the framework include chain.
 *
 * Desired state lives in the shared globals stream_active / stream_deadline
 * (variables.h): POST /api/stream sets them, taskCore1 calls streamService()
 * each loop to reconcile them against the real server, and the auto-off timeout
 * clears them. The config page shows the live stream URL/state by polling
 * GET /api/state, so this module reports nothing back to the UI directly.
 */
#pragma once
#ifndef CAMERA_STREAM_H
#define CAMERA_STREAM_H

#ifdef __cplusplus
extern "C" {
#endif

#define STREAM_PORT 81
#define STREAM_TIMEOUT_MS (5UL * 60UL * 1000UL)  // auto-stop after 5 minutes

// Reconcile desired (stream_active) vs actual server state and enforce the
// auto-off timeout. Call from taskCore1 each loop.
void streamService(void);

#ifdef __cplusplus
}
#endif

#endif // CAMERA_STREAM_H
