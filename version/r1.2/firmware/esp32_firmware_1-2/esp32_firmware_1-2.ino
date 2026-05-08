/**
 * MiniSpeedCam - ESP32 Firmware (Hardware revision 1.2)
 * -----------------------------------------------------
 * Top-level Arduino sketch for the MiniSpeedCam ESP32-S3.
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
 * Required libraries (Arduino IDE):
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
#include "esp_task_wdt.h"      // Per-task watchdog for the two pinned loops
#include "SPIFFS.h"
#include "Base64.h"

// Task watchdog timeout. Long enough to cover HTTPS connect + send (each
// capped at 5s in api.h) plus the 3s wifiResetButton blocking poll, with
// margin. If a task fails to reset the WDT within this window the chip
// panics and reboots, so a wedged radar drain or stuck POST recovers.
#define WDT_TIMEOUT_S 30

// --- GPIO assignments (board revision 1.2) ---
#define STM32_RESET_PIN GPIO_NUM_47  // Drives STM32 NRST low to reset the radar MCU
#define RX_GPIO 42                   // UART1 RX from STM32 (speed reports)
#define TX_GPIO 41                   // UART1 TX to STM32 (speed query commands)
#define ESP_WAKEUP_PIN GPIO_NUM_1    // STM32 pulls HIGH when motion >= ~5mph is detected
#define STM_WAKEUP_PIN GPIO_NUM_2    // Reserved: ESP -> STM wake signal
#define WIFI_RESET_PIN GPIO_NUM_21   // Active-low button: hold 3s to clear stored WiFi creds

#define CAMERA_PWDN_PIN GPIO_NUM_45  // OV2640 power-down (active high)
#define CAMERA_RST_PIN GPIO_NUM_19   // OV2640 reset (active low)

Preferences preferences;  // NVS-backed key/value store for WiFi creds and user settings

#include "variables.h"
#include "camera.h"
#include "radar.h"
#include "api.h"
#include "espui_settings.h"

TaskHandle_t Task0;  // Handle for the Core 0 task (HTTPS uploads / WiFi reconnect)
TaskHandle_t Task1;  // Handle for the Core 1 task (radar polling / sleep / DNS)

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

  // Derive a stable per-device password from the WiFi MAC. 8 hex chars
  // satisfies WPA2's 8-char minimum; same string is reused for ESPUI auth.
  {
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char buf[9];
    snprintf(buf, sizeof(buf), "%02X%02X%02X%02X", mac[2], mac[3], mac[4], mac[5]);
    espui_password = String(buf);
  }
  Serial.print("Soft-AP / ESPUI password: ");
  Serial.println(espui_password);

  // Reconfigure the task watchdog with our timeout before either pinned
  // task subscribes. deinit is safe even if the WDT was never inited.
  esp_task_wdt_deinit();
  esp_task_wdt_init(WDT_TIMEOUT_S, true);

  // GPIO setup
  pinMode(ESP_WAKEUP_PIN, INPUT);
  pinMode(WIFI_RESET_PIN, INPUT);  // Set the wifi reset pin
  pinMode(STM32_RESET_PIN, OUTPUT);
  pinMode(CAMERA_PWDN_PIN, OUTPUT);  // Set the camera powerdown pin
  pinMode(CAMERA_RST_PIN, OUTPUT);   // Set the camera reset pin // Causes crash

  // Light Sleep setup
  //esp_sleep_enable_ext0_wakeup(ESP_WAKEUP_PIN, 1);  // Wake up ESP32 when GPIO1 is HIGH //STM will always pull GIO1 high when speeds above 5 mph are detected. Will pull low when speeds

  // Setup Camera
  cameraPowerOn();
  cameraSetup();

  SPIFFS.begin(true);
  delay(100);

  // load saved variables
  preferences.begin("local", false);
  ssid = preferences.getString("ssid", "NOT_SET");
  password = preferences.getString("pass", "NOT_SET");
  camera_id = preferences.getString("camera_id", "NOT_SET");  // Create an account and camera at tachtracker.com
  min_speed = preferences.getInt("min_speed", 3);             // The minimum speed (MPH) that the tracker should track any vehicle and upload data
  photo_speed = preferences.getInt("photo_speed", 10);        // Cars speed (MPH) when photo should be taken
  is_kph = preferences.getBool("is_kph", 0);                  // Cars speed (MPH) when photo should be taken

  // Connect CDM324 sensor
  Serial.println("Connecting CDM324");
  Serial1.begin(1000000, SERIAL_8N1, RX_GPIO, TX_GPIO);
  // Bound the parseFloat() wait in get_speed() so a missed STM32 reply
  // can't stall the radar polling loop for a full second.
  Serial1.setTimeout(50);
  Serial.setDebugOutput(false);

  // Reset CDM324
  digitalWrite(STM32_RESET_PIN, LOW);
  issue_cdm324_reset();
  // Connect to WiFi or create Access Point
  connectWifiAP();

  // Load ESPUI elements
  load_espui();

  // Send local IP address to API if connected to internet
  sendLocalIP();

  // Post-boot grace window: keep WiFi alive for 120s so the user has time
  // to reach the ESPUI portal before the idle path tears the radio down.
  sleep_time = millis() + 120000;
  wake_flag = true;

  // ignore device measurements for 5 seconds after startup
  ignore_time = millis() + 5000;
  ignore_flag = true;

  xTaskCreatePinnedToCore(
    taskCore1,  // Task function
    "Task1",    // Name of the task
    10000,      // Stack size in words
    NULL,       // Task input parameter
    1,          // Priority of the task
    &Task1,     // Task handle
    1           // Core where the task should run
  );

  /* Place takePhoto function on core 0 */
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

/**
 * Core 1 task: radar polling, sleep policy, and ESPUI/DNS servicing.
 *
 * Each iteration:
 *   1. Services one captive-DNS request so the ESPUI page resolves.
 *   2. Releases the post-startup "ignore" window after 5s.
 *   3. Polls the STM32 for the latest speed (in MPH or KPH per user setting).
 *   4. Manages two sleep/idle paths (post-boot grace, and 5s of no radar).
 *   5. Triggers WiFi reconnect (handed off to Core 0) when the radar wakes.
 *   6. When speed >= min_speed, captures the run's max speed, fires the
 *      camera once at photo_speed, and signals Core 0 to upload.
 *   7. Polls the WiFi-reset button.
 */
void taskCore1(void* parameter) {  // Code for task running on Core 1
  esp_task_wdt_add(NULL);          // Subscribe this task to the watchdog
  while (1) {                      // Loop indefinitely
    esp_task_wdt_reset();          // Heartbeat the watchdog each cycle

    dnsServer.processNextRequest();  // Process request for ESPUI

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

    //Serial.println(speed);  // TESTING

    /* SLEEP - 120 Seconds after startup
     *  Gives time for user to make changes over WiFi
     */

    if (wake_flag == true) {
      if (millis() >= sleep_time) {              // Only if 120 seconds passed
        if (digitalRead(ESP_WAKEUP_PIN) == 0) {  // Only if STM not measuring data
          wake_flag = false;

          // Only tear the radio down when we're actually associated to a
          // network. In AP-fallback mode the user may still be configuring
          // WiFi via the captive portal, so leave the AP up.
          if (WiFi.getMode() != WIFI_AP) {
            Serial.println("Grace window over: powering down WiFi");
            //esp_light_sleep_start(); // Do not sleep in version 1.1
            WiFi.disconnect(true);  // wifioff=true; second arg defaults false so creds are kept
            WiFi.mode(WIFI_OFF);
          } else {
            Serial.println("Grace window over: keeping soft-AP up for configuration");
          }

          // Add power camera off to save battery
          // To initiate hardware power-down, the PWDN pin must be tied to high.
          // Power camera on after speed is detected
          //cameraPowerOn();  // power on camera after waking?
        }
      }
    }

    /* SLEEP - 5 seconds of no activity on radar */
    unsigned long currentMillis = millis();

    if (wake_flag == false) {
      if (speed == 0) {  // Check if speed is 0
        if (currentMillis - previousMillis >= interval) {
          previousMillis = currentMillis;      // Save the last time

          // Same AP-aware guard as the grace-window path above; keep the
          // captive portal alive while the user is configuring.
          if (WiFi.getMode() != WIFI_AP) {
            Serial.println("Idle 5s with no radar activity: powering down WiFi");
            //esp_light_sleep_start(); // Do not Go to sleep on R1.1 because USB will disconnect
            WiFi.disconnect(true);
            WiFi.mode(WIFI_OFF);
          }
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
      if (WiFi.status() != WL_CONNECTED) {
        connect_wifi = true;
      }
      // Power on camera here? Need to check if it's powered down first

    }

    if (ignore_flag == false) {
      if (speed >= min_speed) {

        Serial.println("Min speed triggered");
        Serial.println(min_speed);
        Serial.println(speed);

        delay(100);
        maxSpeed = 0;  // Tracks the max speed during the entire duraton of tracking
        bool collect_data_point = true;
        bool photo_captured = false;
        send_data = false;
        send_photo = false;
        speed_collection_complete = false;  // Don't send data until photo is finished

        while (speed >= min_speed) {  // Capture the cars maximum speed during the entire drive. Then reset back to zero. Send this max speed.
          esp_task_wdt_reset();  // Long passes can exceed WDT_TIMEOUT_S; heartbeat per cycle

          if (is_kph == true) {
            speed = get_speed(true);  // Get speed (KPH) from STM32 via UART
          } else {
            speed = get_speed(false);  // Get speed (MPH) from STM32 via UART
          }

          if (speed > maxSpeed) {
            maxSpeed = speed;
            Serial.print("New maxSpeed: ");
            Serial.println(maxSpeed);
          }

          if ((maxSpeed >= photo_speed) && (collect_data_point == true)) {  // Only take a photo if one is not already in progress
            Serial.println("Taking photo_speed photo");
            photo_captured = takePhoto();
            if (photo_captured) {
              send_data = true;          // Process photo on Core 0
            } else {
              Serial.println("Capture failed; will not retry this run");
            }
            collect_data_point = false;  // give up on the photo for this run either way
          }

          delay(100);
        }

        Serial.print("MAX maxSpeed: ");
        Serial.println(maxSpeed);

        // Only flag a photo upload if we actually have one. A speeding pass
        // with a failed capture falls through to the speed-only endpoint.
        if (maxSpeed >= photo_speed && photo_captured) {
          send_photo = true;
        }

        speed_collection_complete = true;  // Signal to httpsSend task to send data

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
 * reconnect retries) never delays the radar polling loop. Acts on two
 * flags raised by Core 1: send_data triggers an upload, connect_wifi
 * triggers a (re)connection attempt.
 */
void taskCore0(void* parameter) {
  esp_task_wdt_add(NULL);
  while (1) {
    esp_task_wdt_reset();

    if (send_data == true) {
      sendPhoto();
      send_data = false;
    }

    if (connect_wifi == true) {
      connectWifi();
      connect_wifi = false;
    }

    delay(10);
  }
}
