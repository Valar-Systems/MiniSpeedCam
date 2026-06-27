/**
 * camera_stream.cpp - MJPEG "aiming" video stream (see camera_stream.h).
 *
 * Serves a live MJPEG feed from the OV2640 at GET /stream so the installer can
 * frame the device while mounting it -- shown inline in a small window on the
 * config page. It shares the config portal's esp_http_server on port 80 rather
 * than running a second server/port; to keep that single-task server responsive
 * while the (never-ending) video streams, the response runs as an
 * esp_http_server ASYNC handler on its own task (stream_task).
 *
 * Setup aid only: while a viewer is connected, taskCore1 pauses the radar/photo
 * path (it gates on stream_active / streamBusy()) and the streaming task drops
 * the sensor to VGA; on exit it restores UXGA for full-res speed photos. The
 * stream auto-stops after STREAM_TIMEOUT_MS.
 *
 * Kept in its own translation unit (free of variables.h) so it can't multiply-
 * define the shared globals -- the two desired-state flags are reached by extern.
 */
#include "camera_stream.h"

#include <Arduino.h>
#include <string.h>
#include "esp_camera.h"

// Desired-state flags owned by variables.h (set by POST /api/stream). Reached
// by extern so this TU never includes variables.h.
extern volatile bool stream_active;
extern volatile unsigned long stream_deadline;

#define STREAM_BOUNDARY_ID "minispeedcamframe"
static const char* STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" STREAM_BOUNDARY_ID;
static const char* STREAM_BOUNDARY = "\r\n--" STREAM_BOUNDARY_ID "\r\n";
static const char* STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

// Non-NULL while a viewer is being streamed to (one viewer at a time). Set by
// the httpd task when it spawns stream_task, cleared by stream_task on exit.
static volatile TaskHandle_t stream_task_handle = NULL;

bool streamBusy(void) { return stream_task_handle != NULL; }

// Async streaming task: owns the camera frame size for its lifetime (VGA in,
// UXGA out) and pushes JPEG frames until the stream is turned off (toggle or
// auto-off clears stream_active) or the client disconnects.
static void stream_task(void* arg) {
  httpd_req_t* req = (httpd_req_t*)arg;

  sensor_t* s = esp_camera_sensor_get();
  if (s) s->set_framesize(s, FRAMESIZE_VGA);  // smooth video while aiming
  Serial.println("[STREAM] viewer started (async); camera -> VGA");

  esp_err_t res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
  if (res == ESP_OK) httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  const TickType_t frame_interval = pdMS_TO_TICKS(100);  // ~10 fps
  char part_buf[64];
  while (res == ESP_OK && stream_active) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) { res = ESP_FAIL; break; }

    res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
    if (res == ESP_OK) {
      int hlen = snprintf(part_buf, sizeof(part_buf), STREAM_PART, (unsigned)fb->len);
      res = httpd_resp_send_chunk(req, part_buf, hlen);
    }
    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, (const char*)fb->buf, fb->len);
    }
    esp_camera_fb_return(fb);

    if (res != ESP_OK) break;     // client gone
    vTaskDelay(frame_interval);   // pace + yield CPU to Wi-Fi / radar
  }

  httpd_resp_send_chunk(req, NULL, 0);    // terminate the multipart response
  httpd_req_async_handler_complete(req);  // release the borrowed socket

  if (s) s->set_framesize(s, FRAMESIZE_UXGA);  // restore full-res for speed photos
  Serial.println("[STREAM] viewer ended; camera -> UXGA");

  stream_task_handle = NULL;
  vTaskDelete(NULL);
}

// GET /stream handler (runs on the httpd task). Hands the long-lived MJPEG
// response off to stream_task via the async API so the shared :80 server stays
// responsive, then returns immediately.
static esp_err_t stream_mjpeg_handler(httpd_req_t* req) {
  if (!stream_active) {                 // stream not enabled -- don't touch the camera
    httpd_resp_set_status(req, "503 Service Unavailable");
    return httpd_resp_send(req, "stream off", HTTPD_RESP_USE_STRLEN);
  }
  if (stream_task_handle != NULL) {     // one viewer at a time
    httpd_resp_set_status(req, "503 Service Unavailable");
    return httpd_resp_send(req, "stream busy", HTTPD_RESP_USE_STRLEN);
  }

  httpd_req_t* copy = NULL;
  if (httpd_req_async_handler_begin(req, &copy) != ESP_OK) return ESP_FAIL;

  if (xTaskCreate(stream_task, "mjpeg", 4096, copy, 5,
                  (TaskHandle_t*)&stream_task_handle) != pdPASS) {
    Serial.println("[STREAM] task create FAILED");
    httpd_req_async_handler_complete(copy);
    stream_task_handle = NULL;
    return ESP_FAIL;
  }
  return ESP_OK;  // free the httpd task; stream_task owns `copy` now
}

void streamRegisterHandler(httpd_handle_t server) {
  httpd_uri_t stream_uri = { .uri = "/stream", .method = HTTP_GET,
                             .handler = stream_mjpeg_handler, .user_ctx = NULL };
  httpd_register_uri_handler(server, &stream_uri);
}

// Enforce the 5-minute auto-off. The streaming task watches stream_active and
// stops (restoring UXGA) on its own once this clears it. Runs on taskCore1.
void streamService(void) {
  if (stream_active && (long)(millis() - stream_deadline) >= 0) {
    stream_active = false;
    Serial.println("[STREAM] auto-off (timeout)");
  }
}
