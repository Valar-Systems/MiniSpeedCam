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

// Proximity gate (persisted in NVS). The STM32 reports the FFT peak magnitude
// alongside each speed; magnitude tracks received power (~1/r^4), so a distant
// car reads weak while a car on the road reads strong. A run only arms when the
// live magnitude is >= min_signal, which rejects distant cross-traffic that the
// speed/SNR checks can't. 0 = disabled (no proximity gating; default until the
// user calibrates against the live reading on the Status tab).
int min_signal;              // Minimum peak magnitude to arm a run (0 = off)
int photo_signal;            // Shared/default echo-magnitude fire threshold (proximity). Used for a direction whose specific threshold below is 0. 0 = off (legacy: capture at photo_speed).
int photo_signal_front;      // Fire the FRONT-plate shot as an oncoming car's echo rises UP through this (front reflects strong, so RAISE it to catch oncoming closer). 0 = inherit photo_signal.
int photo_signal_rear;       // Fire the REAR-plate shot as a receding car's echo falls DOWN through this (rear reflects weak, so LOWER it to catch receding farther out). 0 = inherit photo_signal.
volatile uint16_t g_last_peak_mag;  // Magnitude from the most recent get_speed() reply
volatile uint16_t g_last_peak_snr;  // FFT peak SNR x10 from the most recent get_speed() reply (0 = no target / older STM32)

// --- Detection-quality counters (telemetry; reported by sendLocalIP) ---
// Lifetime counts of readings the firmware threw away, so the rejection rate is
// visible remotely instead of only on the serial console. g_reject_speed counts
// corrupt/implausible STM32 replies (checksum fail or out-of-range speed -- NOT
// the common "no car" empty reply); g_reject_proximity counts samples that were
// fast enough to arm a run but failed the proximity gate (distant cross-traffic).
volatile uint32_t g_reject_speed;
volatile uint32_t g_reject_proximity;

// Power-Saver Mode (persisted in NVS). When true the radar loop drops WiFi
// during the idle/sleep windows to save power; when false the device never
// sleeps and WiFi stays up (handy for keeping the web UI reachable). Written
// by the ESPUI callback task, read by taskCore1, hence volatile.
volatile bool power_saver;

volatile bool connect_wifi;  // Core 1 -> Core 0: please attempt a WiFi (re)connect

// --- Aiming video stream (temporary setup/mounting tool) ---
// Desired state is toggled from the ESPUI switcher (and cleared by the auto-off
// timeout); taskCore1's streamService() reconciles it against the real server.
volatile bool stream_active = false;    // true = aiming MJPEG stream should be running
volatile unsigned long stream_deadline; // millis() at which the stream auto-stops

// --- Core 1 -> Core 0 upload handoff ---
// One tracking run produces one UploadRequest, posted to uploadQueue when the
// run ends (so speed_actual is final). Core 0 blocks on the queue instead of
// polling flags, and owns `fb`: it must esp_camera_fb_return(fb) after sending.
struct UploadRequest {
  int speed_actual;   // Final max speed for the run, in the configured units
  bool has_photo;     // true if fb holds a captured JPEG to upload
  camera_fb_t* fb;    // PSRAM framebuffer to stream (nullptr when has_photo is false)

  // --- Per-event telemetry (forwarded to the cloud by sendUpload) ---
  // Captured over the whole pass so each reading carries its own context.
  uint8_t  direction;      // 0 = unknown, 1 = approaching (front), 2 = receding (rear)
  uint16_t peak_mag;       // Strongest FFT peak magnitude in the run (closest approach)
  uint16_t peak_snr;       // Strongest FFT peak SNR x10 in the run (detection confidence)
  uint16_t mean_speed_x10; // Mean of the valid speed samples x10 (steady vs braking shape)
  uint16_t frame_count;    // Number of valid radar samples that made up the run
  uint32_t duration_ms;    // Wall-clock length of the run
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
uint16_t labelSpeed;         // ESPUI label showing the live speed reading
uint16_t labelStream;        // ESPUI label showing the aiming-stream URL (or "off")
uint16_t aimingSwitchId;     // ESPUI switcher handle, so streamStop() can flip it off on auto-off
String local_ip_address;     // Most recent station-mode IP (sent to the cloud)

// Captive-DNS config for AP mode. NOTE: dnsServer.start() is never called
// today (only processNextRequest() in taskCore1), so the captive portal
// redirect is currently inactive — wire up dnsServer.start(DNS_PORT, "*", apIP)
// in connectWifiAP()'s AP branch to enable it.
const byte DNS_PORT = 53;                    // Captive DNS port for AP-mode redirection
IPAddress apIP(192, 168, 4, 1);              // Soft-AP gateway / portal IP
