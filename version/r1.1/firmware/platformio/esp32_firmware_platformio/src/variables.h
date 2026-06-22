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
#include "config.h"          // API_BASE_URL / bootstrap token / TLS root used below

// ESP32 firmware version. Normally set by the build flag in platformio.ini;
// this fallback keeps a plain compile working. Reported in the heartbeat and
// used by the OTA flow to decide whether this board needs updating.
#ifndef FW_VERSION
#define FW_VERSION "0.0.0-dev"
#endif

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

// STM32 firmware version, queried over UART at boot (the 'v' command). Stays
// "unknown" on older STM32 firmware that predates 'v'. Reported in the
// heartbeat so the cloud can tell whether the STM32 needs reflashing.
String stm_fw_version = "unknown";

// Power-Saver Mode (persisted in NVS). When true the radar loop drops WiFi
// during the idle/sleep windows to save power; when false the device never
// sleeps and WiFi stays up (handy for keeping the web UI reachable). Written
// by the ESPUI callback task, read by taskCore1, hence volatile.
volatile bool power_saver;

volatile bool connect_wifi;  // Core 1 -> Core 0: please attempt a WiFi (re)connect

// True once boot STA association failed and we fell back to the config soft-AP.
// While set, radar motion must NOT trigger an STA reconnect: connectWifi() would
// tear the portal down chasing an unreachable network, leaving the device with
// neither STA nor AP (the "lost the portal" trap). Cleared on a successful STA
// connect at boot; a reboot re-attempts STA. Set in connectWifiAP().
volatile bool ap_fallback_mode = false;

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
  uint8_t  mag_trend;      // Echo-energy centroid across the run, 0-100 (50 = symmetric pass; >50 approaching, <50 receding; 255 = too few echoes to compute). Direction falls back to this when proximity thresholds are off.
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

// --- Per-device identity (generated once on first boot, persisted in NVS) ---
// One firmware image serves every unit; each device derives its own identity at
// runtime instead of a per-unit build or a user-pasted camera id. device_token
// is the secret that authenticates this unit to the cloud; claim_code is the
// short human code the user types into the MiniSpeedCam web app to link the
// device to their account. Both are minted from the hardware RNG on first boot
// and then reused forever -- never regenerated if already stored, since that
// would orphan the device's cloud record. See loadOrCreateIdentity() in api.h.
String device_token;          // 32-char hex secret identifying this unit to the cloud
String claim_code;            // 6-char human code (A-Z/2-9) shown in the portal for claiming
volatile bool device_claimed; // true once a capture is accepted ({"status":"ok"}); hides the claim code

// --- Sleep / startup gating ---
int sleep_time;              // Absolute millis() at which post-boot grace window ends
bool wake_flag;              // true while the post-boot grace window is active

bool ignore_flag;            // true: drop measurements during the startup blanking window
int ignore_time;             // Absolute millis() at which the blanking window ends

// --- Cloud endpoints (Bubble.io workflow URLs; base from config.h API_BASE_URL) ---
// API_BASE_URL selects version-test (default) vs version-live (build flag). All
// device identity now travels in device_token, not the old "camera" field.
const char* server_register_device  = API_BASE_URL "/register_device";    // POST {mac,device_token,claim_code}: trust-on-first-use registration (idempotent)
const char* server_capture          = API_BASE_URL "/capture";            // POST {device_token,speed_actual,send_photo,photo,...}: {"status":"ok"}=accepted, {}=rejected
const char* server_local_ip_address = API_BASE_URL "/camera";             // POST: announce LAN IP + health heartbeat (keyed by device_token). Workflow renamed local_ip_address -> camera.
const char* server_firmware_check   = API_BASE_URL "/firmware_check";     // POST {device_token,esp_fw,stm_fw} -> latest {esp,stm}_{version,url,md5}

// --- Firmware OTA (manual-approve; triggered from the web-UI buttons) ---
// An ESPUI button callback (AsyncTCP task) sets ota_request and returns; taskCore0
// performs the work and writes a human-readable line into ota_status, which
// taskCore1 mirrors to the web UI. ota_request is an atomic int; ota_status is a
// fixed buffer (a torn cross-task read garbles one status refresh, never crashes).
// The discovered URLs/MD5s are touched only on Core 0, so they need no guarding.
enum { OTA_NONE = 0, OTA_CHECK = 1, OTA_INSTALL_ESP = 2, OTA_INSTALL_STM = 3 };
volatile int ota_request = OTA_NONE;
char ota_status[96] = "idle";
String ota_esp_version, ota_esp_url, ota_esp_md5;
String ota_stm_version, ota_stm_url, ota_stm_md5;
bool ota_esp_available = false;
bool ota_stm_available = false;
// True while Core 0 is reflashing the STM32 (owns UART1 for the ROM bootloader);
// taskCore1 pauses radar polling so the two don't fight over the shared UART.
volatile bool stm_flash_busy = false;

// --- HTTP scratch buffers ---
String payload;              // Last HTTPS response body (debug)
int httpsResponseCode;       // Last HTTPS status code
String httpsRequestData;     // Request-body buffer used by sendLocalIP()

// --- ESPUI control handles ---
uint16_t wifi_ssid_text, wifi_pass_text;  // ESPUI text inputs for WiFi credentials
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
