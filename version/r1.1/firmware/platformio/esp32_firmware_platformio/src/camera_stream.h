/**
 * camera_stream.h - Public API for the temporary MJPEG "aiming" video stream.
 *
 * The stream now shares the config portal's esp_http_server on port 80 (no
 * separate port): load_config_portal() calls streamRegisterHandler() to add a
 * GET /stream route, and the config page embeds it inline in a small window
 * (<img src="/stream">). Because esp_http_server dispatches handlers from a
 * single task, the never-ending MJPEG response is run as an ASYNC handler
 * (httpd_req_async_handler_begin + a dedicated task) so it does not block the
 * config page served by the same server.
 *
 * Desired state lives in the shared globals stream_active / stream_deadline
 * (variables.h): POST /api/stream sets them and taskCore1 calls streamService()
 * each loop to enforce the 5-minute auto-off. The streaming task owns the camera
 * frame size (VGA while a viewer is connected, UXGA otherwise) so the sensor is
 * only ever reconfigured from one task; taskCore1 stays out of the camera while
 * streamBusy() is true.
 *
 * NOTE: do not name this header "stream.h" -- on a case-insensitive filesystem
 * that shadows the Arduino core's own Stream.h and breaks the include chain.
 */
#pragma once
#ifndef CAMERA_STREAM_H
#define CAMERA_STREAM_H

#include "esp_http_server.h"

#define STREAM_TIMEOUT_MS (5UL * 60UL * 1000UL)  // auto-stop after 5 minutes

#ifdef __cplusplus
extern "C" {
#endif

// Register GET /stream (the inline MJPEG aiming feed) on the already-started
// config-portal server. Call once from load_config_portal().
void streamRegisterHandler(httpd_handle_t server);

// Enforce the auto-off timeout (clears stream_active after STREAM_TIMEOUT_MS).
// Call from taskCore1 each loop.
void streamService(void);

// True while a viewer is being streamed to (the async task is alive). taskCore1
// uses this to keep off the camera until the streaming task has fully stopped
// and restored the sensor to UXGA.
bool streamBusy(void);

#ifdef __cplusplus
}
#endif

#endif // CAMERA_STREAM_H
