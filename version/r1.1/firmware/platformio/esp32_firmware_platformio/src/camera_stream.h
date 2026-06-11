/**
 * camera_stream.h - Public API for the temporary MJPEG "aiming" video stream.
 *
 * The implementation (camera_stream.cpp) runs a lightweight esp_http_server and
 * MUST live in its own translation unit: esp_http_server.h and ESPUI's
 * ESPAsyncWebServer both declare a global HTTP_GET / HTTP_POST / ... enum, so
 * they cannot coexist in one file. This header therefore exposes only plain
 * functions (no esp_http_server / ESPUI types) and is safe to include from
 * main.cpp alongside ESPUI.
 *
 * NOTE: do not name the implementation header "stream.h" -- on a case-insensitive
 * filesystem that shadows the Arduino core's own Stream.h (pulled in by Udp.h),
 * which breaks the framework include chain.
 *
 * Desired state lives in the shared globals stream_active / stream_deadline
 * (variables.h): the ESPUI switcher sets them, taskCore1 calls streamService()
 * each loop to reconcile them against the real server, and the auto-off timeout
 * clears them. Start/stop are reported back through the callbacks below so the
 * web UI (an ESPUI concern, kept out of this module) can be updated by main.cpp.
 */
#pragma once
#ifndef CAMERA_STREAM_H
#define CAMERA_STREAM_H

#ifdef __cplusplus
extern "C" {
#endif

#define STREAM_PORT 81
#define STREAM_TIMEOUT_MS (5UL * 60UL * 1000UL)  // auto-stop after 5 minutes

// Notified from streamService() when the server starts (with the stream URL)
// and when it stops (switch-off or auto-off timeout). main.cpp wires these to
// its ESPUI label / switcher updates.
typedef void (*StreamStartedCb)(const char* url);
typedef void (*StreamStoppedCb)(void);
void streamSetCallbacks(StreamStartedCb on_started, StreamStoppedCb on_stopped);

// Reconcile desired (stream_active) vs actual server state and enforce the
// auto-off timeout. Call from taskCore1 each loop.
void streamService(void);

#ifdef __cplusplus
}
#endif

#endif // CAMERA_STREAM_H
