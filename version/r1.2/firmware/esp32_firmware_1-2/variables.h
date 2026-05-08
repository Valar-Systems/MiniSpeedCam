/**
 * variables.h - Shared global state for the MiniSpeedCam ESP32 firmware.
 *
 * All globals used to coordinate between Core 0 (networking) and Core 1
 * (radar polling) live here. Booleans ending in `_flag` or named like
 * `send_*` / `connect_*` are inter-task signals: typically set by Core 1
 * and consumed/cleared by Core 0.
 */

// --- Live measurement / control state ---
float speed = 0.0f;                          // Latest speed sample read from the STM32 (MPH or KPH per is_kph)
bool send_data;                              // Core 1 -> Core 0: a fresh photo is captured, please upload it
int min_speed;                               // Minimum speed (configured units) that begins a tracking run
int photo_speed;                             // Speed threshold within a run that triggers a photo capture
bool connect_wifi;                           // Core 1 -> Core 0: please attempt a WiFi (re)connect
bool speed_collection_complete = false;      // Core 1 sets true once the per-vehicle max speed is finalized
bool is_kph;                                 // true = KPH units, false = MPH (persisted in NVS)
bool send_photo;                             // true = upload included a photo (vehicle was speeding)
float maxSpeed = 0.0f;                       // Highest speed observed during the current tracking run (0.1 resolution from STM32)

// --- WiFi / cloud credentials (persisted in NVS) ---
String ssid;                                 // Stored WiFi SSID, "NOT_SET" until configured
String password;                             // Stored WiFi password, "NOT_SET" until configured
String camera_id;                            // minispeedcam.com camera identifier

// --- Sleep / startup gating ---
unsigned long sleep_time;                    // Absolute millis() at which post-boot grace window ends
bool wake_flag;                              // true while the post-boot grace window is active

bool ignore_flag;                            // true: drop measurements during the startup blanking window
unsigned long ignore_time;                   // Absolute millis() at which the blanking window ends

bool sending_data = false;                   // true while sendPhoto() is mid-flight (uploads are in progress)

// --- Cloud endpoints (Bubble.io workflow URLs on minispeedcam.com) ---
const char* server_speeding = "https://minispeedcam.com/api/1.1/wf/speeding_capture";          // POST: photo + max speed
const char* server_non_speeding = "https://minispeedcam.com/api/1.1/wf/non_speeding_capture";  // POST: max speed only
const char* server_local_ip_address = "https://minispeedcam.com/api/1.1/wf/local_ip_address";  // POST: announce local IP

// --- HTTP scratch buffers ---
String payload;                              // Last HTTPS response body (debug)
int httpsResponseCode;                       // Last HTTPS status code
String httpsRequestData;                     // Reusable request-body buffer used by sendLocalIP()

String photo_base64;                         // Base64-encoded JPEG produced by takePhoto()

String photo_filename;                       // Filename field included in the upload payload
int speed_actual;                            // Snapshot of maxSpeed taken at upload time

// --- ESPUI control handles ---
uint16_t wifi_ssid_text, wifi_pass_text, camera_id_text;  // ESPUI text inputs for credentials
String local_ip_address;                     // Most recent station-mode IP (sent to the cloud)

String hostname = "Radar";                   // mDNS / WiFi station hostname

DNSServer dnsServer;                         // Captive DNS used in AP mode so any URL hits the ESPUI portal

// Per-device password derived from the WiFi MAC. Used both as the soft-AP
// WPA2 PSK and as the ESPUI HTTP basic-auth password (user "admin").
// Printed to Serial at boot; the operator gets it from the device label.
String espui_password;