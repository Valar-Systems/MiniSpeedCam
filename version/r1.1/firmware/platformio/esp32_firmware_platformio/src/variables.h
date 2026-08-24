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

// Installation cosine correction (persisted in NVS, key "speed_corr").
// Doppler reads only the radial component: v_read = v_true * cos(theta),
// where theta is the angle between the radar beam and the car's travel. A
// unit aimed along the road from a lateral offset therefore reads slightly
// LOW by a factor that is fixed for a given mounting -- e.g. a house-corner
// mount 10.5 m off the centerline, measuring 50-70 m up-street, sits at
// 8.6-12 deg off-axis and reads 1.1-2.2% low, so one constant (~1.016)
// corrects it to within +/-0.6%. Applied on the ESP32 (in get_speed(), in
// the float domain before the int truncation) so the STM32 keeps reporting
// the untouched physics measurement for bench validation. It multiplies
// every sample BEFORE the min_speed / photo_speed comparisons, so users set
// thresholds in true road speed. Clamped to [1.00, 1.30]: cosine error can
// only make the radar read low, never high, and 1.30 corresponds to ~40 deg
// off-axis -- beyond that the angle changes too much across the measurement
// zone for any single constant to be honest, and the fix is re-aiming the
// unit along the road, not math.
float speed_correction;      // 1.0 = off (default); set from the config portal

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

// Consecutive get_speed() polls that got NO reply at all from the STM32 -- a
// dead/mute radar link (crash or mid-failed reflash), distinct from a valid
// "no target" reply which still arrives as a checksummed 0-speed line. Reset to
// 0 on any received line. The power-saver paths read this (via stmLinkDead() in
// radar.h) so they don't drop WiFi while the radar is dead -- keeping the portal
// and OTA reachable so the STM can be reflashed instead of the device stranding.
volatile uint32_t g_stm_no_reply_streak = 0;

// STM32 firmware version, queried over UART at boot (the 'v' command). Stays
// "unknown" on older STM32 firmware that predates 'v'. Reported in the
// heartbeat so the cloud can tell whether the STM32 needs reflashing.
//
// A fixed char[] rather than a String on purpose: this value is rewritten on
// taskCore0 after an STM auto-flash while the httpd task may be reading it to
// build /api/state (firmwareVersionText()). A String reassignment frees its old
// heap buffer, which is a use-after-free for that concurrent reader; a fixed
// buffer is only ever overwritten in place, so the worst case is a harmless
// torn read, never a freed-pointer dereference. Sized to hold the 'v' reply
// (radar.h reads at most a 23-char version before NUL).
char stm_fw_version[24] = "unknown";

// Power-Saver Mode (persisted in NVS). When true the radar loop drops WiFi
// during the idle/sleep windows to save power; when false the device never
// sleeps and WiFi stays up (handy for keeping the web UI reachable). Written
// by the config portal's HTTP handler task, read by taskCore1, hence volatile.
volatile bool power_saver;

volatile bool connect_wifi;  // Core 1 -> Core 0: please attempt a WiFi (re)connect

// Core 1 -> Core 0: true while a tracking run (a car pass) is in progress. The
// auto-update service on Core 0 reads this so it never reboots (ESP) or seizes
// UART1 (STM) mid-capture; it only applies firmware between passes. Set at run
// start and cleared when the run's UploadRequest is enqueued (see taskCore1).
volatile bool g_run_active = false;

// Set true on the first successful cloud round-trip since boot (a Bubble 200 or
// a GitHub check). The OTA health watchdog reads this to confirm a freshly-applied
// ESP image can actually reach the network before committing it (see ota.h).
volatile bool g_cloud_ok = false;

// True once boot STA association failed and we fell back to the config soft-AP.
// While set, radar motion must NOT trigger an STA reconnect: connectWifi() would
// tear the portal down chasing an unreachable network, leaving the device with
// neither STA nor AP (the "lost the portal" trap). Cleared on a successful STA
// connect at boot; a reboot re-attempts STA. Set in connectWifiAP().
volatile bool ap_fallback_mode = false;

// --- Aiming video stream (temporary setup/mounting tool) ---
// Desired state is toggled from POST /api/stream (and cleared by the auto-off
// timeout); taskCore1's streamService() reconciles it against the real server.
volatile bool stream_active = false;    // true = aiming MJPEG stream should be running
volatile unsigned long stream_deadline; // millis() at which the stream auto-stops

// --- Core 1 -> Core 0 upload handoff ---
// One tracking run produces one UploadRequest, posted to uploadQueue when the
// run ends (so speed_actual is final). Core 0 blocks on the queue instead of
// polling flags, and owns `photo_buf`: it must free(photo_buf) after sending.
struct UploadRequest {
  int speed_actual;   // Final max speed for the run, in the configured units
  bool has_photo;     // true if photo_buf holds a captured JPEG to upload
  uint8_t* photo_buf; // ps_malloc'd JPEG copy to stream (nullptr when no photo). Core 0 free()s it after
  size_t   photo_len; // byte length of photo_buf. Decoupled from the camera driver's fb pool so a slow
                      // upload can never hold a framebuffer and block a later capturePhoto() on Core 1.

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

// --- Cloud endpoints (Bubble.io workflow URLs; base is runtime-configurable) ---
// api_base_url is the data destination. It defaults to API_BASE_URL (config.h,
// build-flag overridable: version-test vs version-live) but the user can
// re-point it at a different server from the "Data Server" field on the Wifi
// Settings tab; the chosen value is persisted in NVS (key "api_base"). The three
// endpoint Strings are rebuilt from it by rebuildServerEndpoints(), called once
// at boot after the stored value is loaded. A change takes effect on reboot (the
// Save button reboots), so Core 0 never reads an endpoint String mid-edit. All
// device identity travels in device_token, not the old "camera" field.
String api_base_url = API_BASE_URL;     // editable API base (no trailing slash); see rebuildServerEndpoints()
String server_register_device;          // POST {mac,device_token,claim_code}: trust-on-first-use registration (idempotent)
String server_capture;                  // POST {device_token,speed_actual,send_photo,photo,...}: {"status":"ok"}=accepted, {}=rejected
String server_local_ip_address;         // POST: announce LAN IP + health heartbeat (keyed by device_token). Workflow renamed local_ip_address -> camera.

// Derived from api_base_url by rebuildServerEndpoints() (read-only afterwards):
//   g_server_secure   - true  => https:// base, use WiFiClientSecure + TLS policy.
//                       false => http:// base (e.g. a LAN Home Assistant webhook),
//                                use a plain WiFiClient (no TLS handshake).
//   g_server_is_bubble- true  => the minispeedcam.com Bubble backend, which signals
//                                accept/reject as {"status":"ok"} vs an empty {}.
//                       false => a generic endpoint (HA webhook, self-host): any 2xx
//                                means the event was taken (no claim/quota protocol).
// Both Core 0 only, set once at boot, so no cross-task guarding is needed.
bool g_server_secure = true;
bool g_server_is_bubble = true;

// Rebuild the endpoint URLs from api_base_url. Call once at boot after loading
// api_base_url from NVS (and never while Core 0 might be mid-POST -- the Save
// button reboots instead of mutating these live). Trims surrounding whitespace,
// falls back to the compile-time default if the stored base is blank, strips any
// trailing slash so "<base>/" + "/capture" can't double up the separator, and
// derives the transport/semantics flags above from the (case-insensitive) scheme
// and host.
void rebuildServerEndpoints() {
  api_base_url.trim();
  if (api_base_url.isEmpty()) api_base_url = API_BASE_URL;
  while (api_base_url.endsWith("/")) api_base_url.remove(api_base_url.length() - 1);
  server_register_device  = api_base_url + "/register_device";
  server_capture          = api_base_url + "/capture";
  server_local_ip_address = api_base_url + "/camera";

  String lower = api_base_url;
  lower.toLowerCase();
  g_server_secure    = !lower.startsWith("http://");          // anything not explicitly http:// is treated as https
  g_server_is_bubble = (lower.indexOf("minispeedcam.com") >= 0);
}

// --- Firmware OTA source: GitHub Releases (replaces the old Bubble firmware_check) ---
// The device GETs the latest release from the public repo's Releases API, then
// reads the attached manifest.json asset for the per-MCU versions + .bin URLs +
// MD5s (see otaCheckGithub() in ota.h). Owner/repo come from config.h.
const char* server_github_latest = "https://api.github.com/repos/" GITHUB_OWNER "/" GITHUB_REPO "/releases/latest";

// --- Firmware OTA (automatic from GitHub; no user buttons) ---
// taskCore0 polls GitHub on a timer and applies any strictly-newer image while
// the device is idle; it writes a human-readable line into ota_status, which
// taskCore1 mirrors to the web UI. ota_status is a fixed buffer (a torn
// cross-task read garbles one status refresh, never crashes). The discovered
// URLs/MD5s and the *_available flags are touched only on Core 0, so they need
// no guarding.
char ota_status[96] = "idle";
// Manual "check for updates now" request from the config-page button. Set by the
// POST /api/ota/check handler (httpd task) and consumed by otaAutoService() on
// Core 0, which then runs a check immediately instead of waiting for the timer.
volatile bool ota_check_now = false;
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

// The web config portal (config_portal.h) is stateless: it serves settings +
// live values from these globals on demand (GET /api/state) and writes them
// back via POST handlers, so there are no per-control UI handles to track.
String local_ip_address;     // Most recent station-mode IP (sent to the cloud)

// Captive-DNS config for AP mode. NOTE: dnsServer.start() is never called
// today (only processNextRequest() in taskCore1), so the captive portal
// redirect is currently inactive — wire up dnsServer.start(DNS_PORT, "*", apIP)
// in connectWifiAP()'s AP branch to enable it.
const byte DNS_PORT = 53;                    // Captive DNS port for AP-mode redirection
IPAddress apIP(192, 168, 4, 1);              // Soft-AP gateway / portal IP
