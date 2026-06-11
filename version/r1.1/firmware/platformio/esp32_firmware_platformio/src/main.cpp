/**
 * MiniSpeedCam - ESP32 Firmware (Hardware revision 1.1)
 * -----------------------------------------------------
 * Top-level entry point for the MiniSpeedCam ESP32-S3.
 *
 * High-level responsibilities:
 *   - Polls the STM32 (which performs FFT on the CDM324 radar signal)
 *     over UART for the current vehicle speed.
 *   - When speed crosses the user-configured threshold, captures a JPEG
 *     from the OV2640 camera and POSTs it (base64-encoded) along with
 *     the maximum measured speed to minispeedcam.com.
 *   - Hosts an ESPUI web UI (over WiFi STA, or a captive AP if no
 *     credentials are stored) for configuring WiFi, units (MPH/KPH),
 *     minimum tracking speed, and photo trigger speed.
 *   - Implements a two-task split across the dual-core ESP32:
 *       Core 1 (Task1) - radar polling, sleep management, ESPUI/DNS.
 *       Core 0 (Task0) - HTTPS uploads and (re)connecting to WiFi.
 *
 * PlatformIO port of esp32_firmware/esp32_firmware.ino. The module
 * headers below contain their definitions and are compiled as a single
 * translation unit (exactly as the Arduino IDE concatenated the .ino
 * tabs), so behaviour is identical to the original sketch.
 *
 * Required libraries (see platformio.ini lib_deps):
 *   - Async TCP 3.3.8
 *   - ESPUI    2.2.4
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <esp_camera.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <ESPUI.h>
#include <Preferences.h>

#include "soc/soc.h"           // Disable brownout problems
#include "soc/rtc_cntl_reg.h"  // Disable brownout problems
#include "driver/rtc_io.h"
#include "SPIFFS.h"

// --- GPIO assignments (board revision 1.1) ---
#define STM32_RESET_PIN GPIO_NUM_47  // Drives STM32 NRST low to reset the radar MCU
#define RX_GPIO 42                   // UART1 RX from STM32 (speed reports)
#define TX_GPIO 41                   // UART1 TX to STM32 (speed query commands)
#define ESP_WAKEUP_PIN GPIO_NUM_1    // STM32 pulls HIGH when motion >= ~5mph is detected
#define STM_WAKEUP_PIN GPIO_NUM_2    // Reserved: ESP -> STM wake signal
#define WIFI_RESET_PIN GPIO_NUM_21   // Active-low button: hold 3s to clear stored WiFi creds

#define CAMERA_PWDN_PIN GPIO_NUM_45  // OV2640 power-down (active high)
#define CAMERA_RST_PIN GPIO_NUM_3    // OV2640 reset (active low)

Preferences preferences;  // NVS-backed key/value store for WiFi creds and user settings

#include "variables.h"
#include "diagnostics.h"
#include "camera.h"
#include "camera_stream.h"
#include "radar.h"
#include "api.h"
#include "espui_settings.h"

DNSServer dnsServer;  // Captive-DNS used in AP mode so any URL hits the ESPUI portal

TaskHandle_t Task0;  // Handle for the Core 0 task (HTTPS uploads / WiFi reconnect)
TaskHandle_t Task1;  // Handle for the Core 1 task (radar polling / sleep / DNS)

// Forward declarations for the dual-core task functions (defined below).
void taskCore0(void* parameter);
void taskCore1(void* parameter);

// --- Aiming-stream <-> ESPUI bridge -----------------------------------------
// The MJPEG stream server lives in its own translation unit (camera_stream.cpp)
// because esp_http_server and ESPUI's ESPAsyncWebServer can't share a file
// (clashing HTTP_* enums). It reports start/stop through these callbacks so the
// ESPUI label/switcher (an ESPUI concern, kept here) stay in sync.
static void onStreamStarted(const char* url) {
  ESPUI.updateLabel(labelStream, String(url));
}
static void onStreamStopped() {
  ESPUI.updateLabel(labelStream, "off");
  ESPUI.updateSwitcher(aimingSwitchId, false);  // reflect auto-off in the web UI
}

/**
 * Arduino setup() - runs once on boot/reset.
 *
 * Initializes peripherals, restores user settings from NVS, brings the
 * STM32 radar out of reset, joins WiFi (or starts the configuration AP),
 * launches the ESPUI web UI, and finally pins the two work loops to
 * separate cores so radar polling never blocks on HTTPS uploads.
 */
void setup() {
  Serial.begin(115200);
  // On native-USB CDC the port enumerates ~1-2s after reset, so early prints
  // are lost if we don't wait for the host to attach. Bound the wait so the
  // device never hangs when running headless (no monitor) in the field.
  unsigned long serialWaitStart = millis();
  while (!Serial && millis() - serialWaitStart < 2000) { delay(10); }

  Serial.println();
  Serial.println("=== MiniSpeedCam r1.1 (PlatformIO) ===");
  Serial.printf("[BOOT] heap=%u psram=%u\n", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());

  // GPIO setup
  pinMode(ESP_WAKEUP_PIN, INPUT);
  pinMode(WIFI_RESET_PIN, INPUT_PULLUP);  // Active-low button; enable internal pull-up so the pin idles HIGH (no spurious resets)
  pinMode(STM32_RESET_PIN, OUTPUT);
  pinMode(CAMERA_PWDN_PIN, OUTPUT);  // Set the camera powerdown pin
  pinMode(CAMERA_RST_PIN, OUTPUT);   // Set the camera reset pin // Causes crash

  // Light Sleep setup
  //esp_sleep_enable_ext0_wakeup(ESP_WAKEUP_PIN, 1);  // Wake up ESP32 when GPIO1 is HIGH //STM will always pull GIO1 high when speeds above 5 mph are detected. Will pull low when speeds

  // Setup Camera. esp_camera_init() can fail transiently on a marginal power
  // rail, so power-cycle and retry a few times rather than booting blind with
  // a dead sensor (which would silently upload empty photos).
  cameraPowerOn();
  int camera_attempts = 0;
  while (cameraSetup() == 0 && camera_attempts < 3) {
    camera_attempts++;
    Serial.printf("[CAM] init failed, retry %d/3\n", camera_attempts);
    esp_camera_deinit();  // release any partial init before re-attempting
    delay(250);
    cameraPowerOn();
  }
  if (camera_attempts >= 3) {
    Serial.println("[CAM] FAILED after 3 attempts; photo capture disabled");
  } else {
    Serial.println("[CAM] ready");
  }

  SPIFFS.begin(true);
  delay(100);

  // load saved variables
  preferences.begin("local", false);
  diagnosticsLogBoot();  // record reset reason + bump persisted boot counter for the Status tab
  ssid = preferences.getString("ssid", "NOT_SET");
  password = preferences.getString("pass", "NOT_SET");
  camera_id = preferences.getString("camera_id", "NOT_SET");  // Create an account and camera at tachtracker.com
  min_speed = preferences.getInt("min_speed", 3);             // The minimum speed (MPH) that the tracker should track any vehicle and upload data
  photo_speed = preferences.getInt("photo_speed", 10);        // Cars speed (MPH) when photo should be taken
  is_kph = preferences.getBool("is_kph", 0);                  // Cars speed (MPH) when photo should be taken
  power_saver = preferences.getBool("power_saver", true);     // true = drop WiFi during idle (default); false = never sleep

  // Connect CDM324 sensor
  Serial.println("Connecting CDM324");
  Serial1.begin(1000000, SERIAL_8N1, RX_GPIO, TX_GPIO);
  Serial.setDebugOutput(false);

  // Reset CDM324
  digitalWrite(STM32_RESET_PIN, LOW);
  issue_cdm324_reset();
  // Connect to WiFi or create Access Point
  connectWifiAP();
  if (WiFi.getMode() == WIFI_STA) {
    Serial.printf("[WIFI] STA ip=%s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.printf("[WIFI] AP ip=%s\n", WiFi.softAPIP().toString().c_str());
  }

  // Load ESPUI elements
  load_espui();

  // Wire the stream module's start/stop notifications to the ESPUI UI now that
  // the controls (labelStream / aimingSwitchId) exist.
  streamSetCallbacks(onStreamStarted, onStreamStopped);

  // Send local IP address to API if connected to internet
  sendLocalIP();

  // Put device to sleep after 120 seconds after setup
  sleep_time = millis() + 10000;  //120000
  wake_flag = true;

  // ignore device measurements for 5 seconds after startup
  ignore_time = millis() + 5000;
  ignore_flag = true;

  // Queue used to hand finished tracking runs from Core 1 to Core 0. Depth 2
  // lets a second event arrive while one is still uploading; deeper backlogs
  // are dropped (with a log) in taskCore1.
  uploadQueue = xQueueCreate(2, sizeof(UploadRequest));

  Serial.printf("[BOOT] setup complete (heap=%u); starting tasks\n", (unsigned)ESP.getFreeHeap());

  xTaskCreatePinnedToCore(
    taskCore1,  // Task function
    "Task1",    // Name of the task
    10000,      // Stack size in words
    NULL,       // Task input parameter
    1,          // Priority of the task
    &Task1,     // Task handle
    1           // Core where the task should run
  );

  /* Place the network/upload task on core 0 */
  xTaskCreatePinnedToCore(
    taskCore0,  // Task function
    "Task0",    // Name of the task
    20000,      // Stack size in words
    NULL,       // Task input parameter
    1,          // Priority of the task (0 is lowest)
    &Task0,     // Task handle
    0           // Core where the task should run (0 or 1)
  );
}

/**
 * Arduino loop() - intentionally empty.
 *
 * All recurring work is performed in taskCore0 and taskCore1, which are
 * pinned to separate cores via xTaskCreatePinnedToCore() in setup().
 */
void loop() {
  // Loop not used. Use tasks intead for dual-core performance
}

unsigned long previousMillis = 0;  // Timestamp of last idle-sleep tick
const long interval = 5000;        // Idle window (ms) of zero-speed before sleeping

// A run only starts after this many consecutive readings >= min_speed. A lone
// noise spike that slips under the plausibility ceiling (see get_speed) lasts
// a single sample, so requiring 2 in a row rejects glitches while a real
// vehicle (many samples) still triggers immediately.
const int SPEED_CONFIRM_COUNT = 2;

/**
 * Core 1 task: radar polling, sleep policy, and ESPUI/DNS servicing.
 *
 * Each iteration:
 *   1. Services one captive-DNS request so the ESPUI page resolves.
 *   2. Releases the post-startup "ignore" window after 5s.
 *   3. Polls the STM32 for the latest speed (in MPH or KPH per user setting).
 *   4. Manages two sleep/idle paths (post-boot grace, and 5s of no radar).
 *   5. Triggers WiFi reconnect (handed off to Core 0) when the radar wakes.
 *   6. When speed >= min_speed, tracks the run's max speed, captures one
 *      frame once at photo_speed, and posts a single UploadRequest to
 *      Core 0 via uploadQueue when the run ends.
 *   7. Polls the WiFi-reset button.
 */
void taskCore1(void* parameter) {  // Code for task running on Core 1
  while (1) {                      // Loop indefinitely

    dnsServer.processNextRequest();  // Process request for ESPUI

    streamService();  // start/stop the aiming MJPEG stream + enforce its auto-off timeout

    if (ignore_flag == true) {
      if (millis() >= ignore_time) {
        ignore_flag = false;  // Only if 5 seconds passed
      }
    }

    if (is_kph == true) {
      speed = get_speed(true);  // Get speed (KPH) from STM32 via UART
    } else {
      speed = get_speed(false);  // Get speed (MPH) from STM32 via UART
    }

    updateSpeedDisplay();   // push the live reading to the ESPUI "Current Speed" label
    updateStatusDisplay();  // refresh the ESPUI Status tab (~1 Hz, self-throttled)

    // Echo the live speed to the serial monitor, throttled to ~1 Hz so the
    // fast radar-poll loop doesn't flood the console.
    static unsigned long lastSpeedPrint = 0;
    if (millis() - lastSpeedPrint >= 1000) {
      lastSpeedPrint = millis();
      Serial.printf("[SPEED] %d %s\n", speed, is_kph ? "kph" : "mph");
    }

    /* SLEEP - 120 Seconds after startup
     *  Gives time for user to make changes over WiFi.
     *  Skipped entirely when Power-Saver Mode is off (device stays awake).
     */

    // Never power down WiFi while unconfigured: the soft-AP must stay up so
    // the user can open the portal and enter credentials (ssid == "NOT_SET").
    if (power_saver && !stream_active && ssid != "NOT_SET" && wake_flag == true) {
      if (millis() >= sleep_time) {              // Only if 120 seconds passed
        if (digitalRead(ESP_WAKEUP_PIN) == 0) {  // Only if STM not measuring data
          wake_flag = false;
          Serial.println("[PWR] idle: WiFi off (post-boot grace elapsed)");

          //esp_light_sleep_start(); // Do not sleep in version 1.1
          WiFi.disconnect(true);  // Disconnect from network, optionally true to remove credentials
          WiFi.mode(WIFI_OFF);    // Set Wi-Fi mode to OFF

          // Add power camera off to save battery
          // To initiate hardware power-down, the PWDN pin must be tied to high.
          // Power camera on after speed is detected
          //cameraPowerOn();  // power on camera after waking?
        }
      }
    }

    /* SLEEP - 5 seconds of no activity on radar (Power-Saver Mode only) */
    unsigned long currentMillis = millis();

    if (power_saver && !stream_active && ssid != "NOT_SET" && wake_flag == false) {
      if (speed == 0) {  // Check if speed is 0
        if (currentMillis - previousMillis >= interval) {
          previousMillis = currentMillis;      // Save the last time
          Serial.println("[PWR] idle: WiFi off (no radar activity 5s)");

          //esp_light_sleep_start(); // Do not Go to sleep on R1.1 because USB will disconnect
          WiFi.disconnect(true);  // Disconnect from network, optionally true to remove credentials
          WiFi.mode(WIFI_OFF);    // Set Wi-Fi mode to OFF
          previousMillis = millis();

          // Add power camera off to save battery
          // To initiate hardware power-down, the PWDN pin must be tied to high.
          // Power camera on after speed is detected
          //cameraPowerOn();  // power on camera after waking?
        }
      }
    }

    // Checks if wifi is disconnected and sets connect flag to true. Connection will occur on Core 0
    if (digitalRead(ESP_WAKEUP_PIN) == 1) { // Check if speed detected on radar first. Use interrupt for this instead?
      // Only request a reconnect when we actually have credentials to use.
      // In AP config mode (ssid == "NOT_SET") WiFi.status() is never
      // WL_CONNECTED, so without this guard we'd flag a reconnect every loop
      // and connectWifi() would tear the soft-AP down.
      if (ssid != "NOT_SET" && !ssid.isEmpty() && WiFi.status() != WL_CONNECTED) {
        connect_wifi = true;
      }
      // Power on camera here? Need to check if it's powered down first

    }

    // Pause speed tracking while the aiming stream is up: the device is being
    // mounted (not measuring), and the stream task owns the camera framebuffer.
    if (ignore_flag == false && !stream_active) {
      // Debounce: count consecutive over-threshold readings; a single glitch
      // resets the count, so it can never reach SPEED_CONFIRM_COUNT.
      static int speedConfirmCount = 0;
      if (speed >= min_speed) {
        speedConfirmCount++;
      } else {
        speedConfirmCount = 0;
      }

      if (speed >= min_speed && speedConfirmCount >= SPEED_CONFIRM_COUNT) {
        speedConfirmCount = 0;  // consumed; the run loop below tracks the pass

        Serial.printf("[RUN] start: speed=%d >= min_speed=%d (confirmed)\n", speed, min_speed);

        delay(100);
        maxSpeed = 0;               // Tracks the max speed over the entire pass
        bool photo_taken = false;   // ensures we capture at most one frame per run
        camera_fb_t* fb = nullptr;  // held framebuffer, handed to Core 0 for streaming

        while (speed >= min_speed) {  // Track the car's maximum speed over the whole pass.
          speed = get_speed(is_kph);  // Get speed (KPH or MPH per setting) from STM32 via UART
          updateSpeedDisplay();       // keep the ESPUI readout live during the pass

          if (speed > maxSpeed) {
            maxSpeed = speed;
            Serial.print("New maxSpeed: ");
            Serial.println(maxSpeed);
          }

          if ((maxSpeed >= photo_speed) && (photo_taken == false)) {  // Capture once per run
            Serial.println("Taking photo_speed photo");
            fb = capturePhoto();  // keep the framebuffer; Core 0 returns it after upload
            photo_taken = true;
          }

          delay(100);
        }

        // The run has ended, so maxSpeed is final. Hand it to Core 0 as a
        // single upload request. A photo is attached only if a capture
        // actually succeeded; otherwise it uploads speed-only.
        UploadRequest req;
        req.speed_actual = maxSpeed;
        req.has_photo = (fb != nullptr);
        req.fb = fb;
        Serial.printf("[RUN] end: maxSpeed=%d has_photo=%d -> enqueue\n", maxSpeed, (int)req.has_photo);
        if (xQueueSend(uploadQueue, &req, 0) != pdTRUE) {
          Serial.println("[RUN] upload queue FULL, dropping event");
          if (fb != nullptr) {
            esp_camera_fb_return(fb);  // avoid leaking the framebuffer
          }
        }

        previousMillis = millis();
      }
    }

    // Processes the WiFi reset button. Clears Wifi data if pressed for 3 seconds
    wifiResetButton();

    delay(100);
  }
}

/**
 * Core 0 task: networking I/O.
 *
 * Kept off Core 1 so the relatively slow HTTPS upload (and any WiFi
 * reconnect retries) never delays the radar polling loop. Blocks on
 * uploadQueue for finished-run events from Core 1 (no busy polling); the
 * short receive timeout also lets it service proactive WiFi reconnects
 * requested via connect_wifi.
 */
void taskCore0(void* parameter) {
  while (1) {

    UploadRequest req;
    if (xQueueReceive(uploadQueue, &req, pdMS_TO_TICKS(200)) == pdTRUE) {
      Serial.printf("[UPLOAD] dequeued event: speed=%d has_photo=%d\n", req.speed_actual, (int)req.has_photo);
      sendUpload(req);
      if (req.fb != nullptr) {
        esp_camera_fb_return(req.fb);  // release the framebuffer Core 1 captured
      }
    }

    if (connect_wifi == true) {
      Serial.println("[WIFI] reconnect requested");
      connectWifi();
      connect_wifi = false;
    }
  }
}
