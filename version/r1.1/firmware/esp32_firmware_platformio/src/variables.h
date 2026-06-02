/**
 * variables.h - Shared global state for the MiniSpeedCam ESP32 firmware.
 *
 * Core 1 (radar polling) hands finished tracking runs to Core 0 (networking)
 * through `uploadQueue` rather than a set of polled flags. Most state below
 * is therefore single-core; `connect_wifi` is the one remaining cross-core
 * flag (declared volatile so its check is never optimised away).
 */

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"  // QueueHandle_t for the Core 1 -> Core 0 upload handoff

// --- Live measurement / control state (Core 1 only) ---
int speed;                   // Latest speed sample read from the STM32 (MPH or KPH per is_kph)
int min_speed;               // Minimum speed (configured units) that begins a tracking run
int photo_speed;             // Speed threshold within a run that triggers a photo capture
bool is_kph;                 // true = KPH units, false = MPH (persisted in NVS)
int maxSpeed;                // Highest speed observed during the current tracking run

volatile bool connect_wifi;  // Core 1 -> Core 0: please attempt a WiFi (re)connect

// --- Core 1 -> Core 0 upload handoff ---
// One tracking run produces one UploadRequest, posted to uploadQueue when the
// run ends (so speed_actual is final). Core 0 blocks on the queue instead of
// polling flags, and owns `fb`: it must esp_camera_fb_return(fb) after sending.
struct UploadRequest {
  int speed_actual;   // Final max speed for the run, in the configured units
  bool has_photo;     // true if fb holds a captured JPEG to upload
  camera_fb_t* fb;    // PSRAM framebuffer to stream (nullptr when has_photo is false)
};
QueueHandle_t uploadQueue;  // created in setup()

// --- WiFi / cloud credentials (persisted in NVS) ---
String ssid;                 // Stored WiFi SSID, "NOT_SET" until configured
String password;             // Stored WiFi password, "NOT_SET" until configured
String camera_id;            // minispeedcam.com camera identifier

// --- Sleep / startup gating ---
int sleep_time;              // Absolute millis() at which post-boot grace window ends
bool wake_flag;              // true while the post-boot grace window is active

bool ignore_flag;            // true: drop measurements during the startup blanking window
int ignore_time;             // Absolute millis() at which the blanking window ends

// --- Cloud endpoints (Bubble.io workflow URLs on minispeedcam.com) ---
const char* server_capture = "https://minispeedcam.com/api/1.1/wf/capture";                    // POST: max speed (+ photo when speeding); send_photo field selects the case
const char* server_local_ip_address = "https://minispeedcam.com/api/1.1/wf/local_ip_address";  // POST: announce local IP

// --- HTTP scratch buffers ---
String payload;              // Last HTTPS response body (debug)
int httpsResponseCode;       // Last HTTPS status code
String httpsRequestData;     // Request-body buffer used by sendLocalIP()

// --- ESPUI control handles ---
uint16_t wifi_ssid_text, wifi_pass_text, camera_id_text;  // ESPUI text inputs for credentials
String local_ip_address;     // Most recent station-mode IP (sent to the cloud)

// Captive-DNS config for AP mode. NOTE: dnsServer.start() is never called
// today (only processNextRequest() in taskCore1), so the captive portal
// redirect is currently inactive — wire up dnsServer.start(DNS_PORT, "*", apIP)
// in connectWifiAP()'s AP branch to enable it.
const byte DNS_PORT = 53;                    // Captive DNS port for AP-mode redirection
IPAddress apIP(192, 168, 4, 1);              // Soft-AP gateway / portal IP
