/**
 * camera_stream.cpp - MJPEG "aiming" video stream server (see camera_stream.h).
 *
 * Spins up a lightweight esp_http_server on port 81 that serves a live MJPEG
 * feed from the OV2640 so the installer can frame the device on a phone
 * (open http://<device-ip>:81/). It is a setup aid only: while active the
 * radar/photo path is paused (taskCore1 gates on stream_active) and WiFi is
 * kept up, and it auto-stops after STREAM_TIMEOUT_MS.
 *
 * Compiled as its own translation unit so esp_http_server.h never meets ESPUI's
 * ESPAsyncWebServer (their HTTP_* enums collide). This file therefore must NOT
 * include ESPUI.h, nor variables.h (which both pulls ESPUI in via the main
 * sketch and *defines* the shared globals -- including it here would multiply
 * those definitions). The two desired-state flags are reached by extern, and
 * the web-UI updates are delegated to main.cpp via the callbacks below.
 */
#include "camera_stream.h"

#include <Arduino.h>
#include <WiFi.h>
#include <string.h>
#include "esp_camera.h"
#include "esp_http_server.h"

// Desired-state flags owned by variables.h (set by the ESPUI switcher, read
// across the app). Reached by extern so this TU stays free of ESPUI.
extern volatile bool stream_active;
extern volatile unsigned long stream_deadline;

#define STREAM_BOUNDARY_ID "minispeedcamframe"
static const char* STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" STREAM_BOUNDARY_ID;
static const char* STREAM_BOUNDARY = "\r\n--" STREAM_BOUNDARY_ID "\r\n";
static const char* STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static httpd_handle_t stream_httpd = NULL;
static bool stream_running = false;  // actual server state (reconciled against stream_active)

static StreamStartedCb s_on_started = NULL;
static StreamStoppedCb s_on_stopped = NULL;

void streamSetCallbacks(StreamStartedCb on_started, StreamStoppedCb on_stopped) {
  s_on_started = on_started;
  s_on_stopped = on_stopped;
}

// Minimal phone-friendly page that shows the live feed full width.
static esp_err_t stream_index_handler(httpd_req_t* req) {
  static const char* page =
    "<!doctype html><html><head><meta name=viewport "
    "content='width=device-width,initial-scale=1'><title>MiniSpeedCam Aim</title></head>"
    "<body style='margin:0;background:#000'>"
    "<img src='/stream' style='width:100%;height:auto;display:block'></body></html>";
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, page, strlen(page));
}

// MJPEG multipart stream. Runs in the httpd server task; loops until the
// client disconnects (a chunk send fails).
static esp_err_t stream_mjpeg_handler(httpd_req_t* req) {
  esp_err_t res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
  if (res != ESP_OK) return res;
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  char part_buf[64];
  while (true) {
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

    if (res != ESP_OK) break;  // client gone
  }
  return res;
}

// Start the stream server and switch the sensor to VGA for a smooth feed.
static void streamStart() {
  if (stream_running) return;

  sensor_t* s = esp_camera_sensor_get();
  if (s) s->set_framesize(s, FRAMESIZE_VGA);  // smooth video while aiming

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = STREAM_PORT;
  config.ctrl_port = 32769;       // distinct from the default httpd control port
  config.max_open_sockets = 2;    // keep socket pressure low alongside ESPUI/AsyncTCP
  config.max_uri_handlers = 2;

  if (httpd_start(&stream_httpd, &config) == ESP_OK) {
    httpd_uri_t index_uri  = { .uri = "/",       .method = HTTP_GET, .handler = stream_index_handler, .user_ctx = NULL };
    httpd_uri_t stream_uri = { .uri = "/stream", .method = HTTP_GET, .handler = stream_mjpeg_handler,  .user_ctx = NULL };
    httpd_register_uri_handler(stream_httpd, &index_uri);
    httpd_register_uri_handler(stream_httpd, &stream_uri);
    stream_running = true;

    String ip = (WiFi.getMode() == WIFI_STA) ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
    String url = "http://" + ip + ":" + STREAM_PORT + "/";
    Serial.printf("[STREAM] started at %s (auto-off in %lus)\n", url.c_str(), STREAM_TIMEOUT_MS / 1000);
    if (s_on_started) s_on_started(url.c_str());
  } else {
    Serial.println("[STREAM] httpd_start FAILED");
    if (s) s->set_framesize(s, FRAMESIZE_UXGA);  // restore on failure
    stream_active = false;
  }
}

// Stop the stream server and restore the sensor to UXGA for speed photos.
static void streamStop() {
  if (!stream_running) return;
  httpd_stop(stream_httpd);
  stream_httpd = NULL;
  stream_running = false;

  sensor_t* s = esp_camera_sensor_get();
  if (s) s->set_framesize(s, FRAMESIZE_UXGA);  // back to full-res for speed photos

  Serial.println("[STREAM] stopped; camera restored to UXGA");
  if (s_on_stopped) s_on_stopped();
}

// Reconcile desired (stream_active) vs actual server state, and enforce the
// auto-off timeout. Call from taskCore1 each loop.
void streamService() {
  if (stream_active && (long)(millis() - stream_deadline) >= 0) {
    stream_active = false;  // timeout elapsed
    Serial.println("[STREAM] auto-off (timeout)");
  }
  if (stream_active && !stream_running) {
    streamStart();
  } else if (!stream_active && stream_running) {
    streamStop();
  }
}
