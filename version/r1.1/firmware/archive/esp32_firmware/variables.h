/**
 * variables.h - Shared global state for the MiniSpeedCam ESP32 firmware.
 *
 * All globals used to coordinate between Core 0 (networking) and Core 1
 * (radar polling) live here. Booleans ending in `_flag` or named like
 * `send_*` / `connect_*` are inter-task signals: typically set by Core 1
 * and consumed/cleared by Core 0.
 */

// --- Live measurement / control state ---
int speed;                                   // Latest speed sample read from the STM32 (MPH or KPH per is_kph)
bool API_photo;                              // (reserved) photo-via-API flag
bool send_data;                              // Core 1 -> Core 0: a fresh photo is captured, please upload it
int min_speed;                               // Minimum speed (configured units) that begins a tracking run
int photo_speed;                             // Speed threshold within a run that triggers a photo capture
bool connect_wifi;                           // Core 1 -> Core 0: please attempt a WiFi (re)connect
bool send_API;                               // (reserved) generic API-send flag
bool photo_finished;                         // (reserved) photo-capture-complete flag
bool speed_collection_complete = false;      // Core 1 sets true once the per-vehicle max speed is finalized
bool is_kph;                                 // true = KPH units, false = MPH (persisted in NVS)
bool send_photo;                             // true = upload included a photo (vehicle was speeding)
int maxSpeed;                                // Highest speed observed during the current tracking run

// --- WiFi / cloud credentials (persisted in NVS) ---
String ssid;                                 // Stored WiFi SSID, "NOT_SET" until configured
String password;                             // Stored WiFi password, "NOT_SET" until configured
String camera_id;                            // minispeedcam.com camera identifier

// --- Sleep / startup gating ---
int sleep_time;                              // Absolute millis() at which post-boot grace window ends
bool wake_flag;                              // true while the post-boot grace window is active
int sleep_idle_time;                         // (reserved) idle-sleep timestamp

bool ignore_flag;                            // true: drop measurements during the startup blanking window
int ignore_time;                             // Absolute millis() at which the blanking window ends

bool sending_data = false;                   // true while sendPhoto() is mid-flight (uploads are in progress)

// --- Cloud endpoints (Bubble.io workflow URLs on minispeedcam.com) ---
const char* server_speeding = "https://minispeedcam.com/api/1.1/wf/speeding_capture";          // POST: photo + max speed
const char* server_non_speeding = "https://minispeedcam.com/api/1.1/wf/non_speeding_capture";  // POST: max speed only
const char* server_local_ip_address = "https://minispeedcam.com/api/1.1/wf/local_ip_address";  // POST: announce local IP

// the following variables are unsigned longs because the time, measured in
// milliseconds, will quickly become a bigger number than can be stored in an int.
unsigned long lastTime = 0;

// --- HTTP scratch buffers ---
String payload;                              // Last HTTPS response body (debug)
int httpsResponseCode;                       // Last HTTPS status code
String httpsRequestData;                     // Reusable request-body buffer (reserved up to ~150KB for photo POSTs)

String photo_base64;                         // Base64-encoded JPEG produced by takePhoto()

String photo_filename;                       // Filename field included in the upload payload
int speed_actual;                            // Snapshot of maxSpeed taken at upload time

bool send_alert;                             // (reserved) alert/notification flag

// --- ESPUI control handles ---
uint16_t labelWifi;                          // ESPUI label showing current WiFi status
uint16_t wifi_ssid_text, wifi_pass_text, camera_id_text;  // ESPUI text inputs for credentials
String display_wifi;                         // Cached string used to update the WiFi label
String local_ip_address;                     // Most recent station-mode IP (sent to the cloud)

const byte DNS_PORT = 53;                    // Captive DNS port for AP-mode redirection
IPAddress apIP(192, 168, 4, 1);              // Soft-AP gateway / portal IP

String hostname = "Radar";                   // mDNS / WiFi station hostname

// --- Misc ESPUI handles ---
uint16_t button1;
uint16_t switchOne;
uint16_t status;