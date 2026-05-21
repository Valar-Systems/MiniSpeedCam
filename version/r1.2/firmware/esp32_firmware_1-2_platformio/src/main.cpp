/**
 * MiniSpeedCam - ESP32 Firmware (Hardware revision 1.2)
 * -----------------------------------------------------
 * PlatformIO entry point for the MiniSpeedCam ESP32-S3.
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
 */

#include <Arduino.h>
#include <WiFi.h>
#include <DNSServer.h>

#include "pins.h"
#include "variables.h"
#include "camera.h"
#include "radar.h"
#include "api.h"
#include "espui_settings.h"

DNSServer dnsServer;  // Captive-DNS used in AP mode so any URL hits the ESPUI portal

TaskHandle_t Task0;  // Handle for the Core 0 task (HTTPS uploads / WiFi reconnect)
TaskHandle_t Task1;  // Handle for the Core 1 task (radar polling / sleep / DNS)

void taskCore0(void* parameter);
void taskCore1(void* parameter);

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

  // GPIO setup
  pinMode(ESP_WAKEUP_PIN, INPUT);
  pinMode(WIFI_RESET_PIN, INPUT_PULLUP);  // Active-low button to clear stored WiFi creds
  pinMode(STM32_RESET_PIN, OUTPUT);
  pinMode(CAMERA_PWDN_PIN, OUTPUT);  // Set the camera powerdown pin
  pinMode(CAMERA_RST_PIN, OUTPUT);   // Set the camera reset pin // Causes crash

  // Light Sleep setup
  esp_sleep_enable_ext0_wakeup(ESP_WAKEUP_PIN, 1);  // Wake up ESP32 when GPIO1 is HIGH //STM will always pull GIO1 high when speeds above 5 mph are detected. Will pull low when speeds

  // Setup Camera
  cameraPowerOn();
  cameraSetup();

  // load saved variables
  preferences.begin("local", false);
  net.ssid        = preferences.getString("ssid", "NOT_SET");
  net.password    = preferences.getString("pass", "NOT_SET");
  net.camera_id   = preferences.getString("camera_id", "NOT_SET");  // Create an account and camera at tachtracker.com
  radar.min_speed   = preferences.getInt("min_speed", 3);           // The minimum speed (MPH) that the tracker should track any vehicle and upload data
  radar.photo_speed = preferences.getInt("photo_speed", 10);        // Cars speed (MPH) when photo should be taken
  radar.is_kph      = preferences.getBool("is_kph", 0);             // Cars speed (MPH) when photo should be taken

  // Connect CDM324 sensor
  Serial.println("Connecting CDM324");
  Serial1.begin(1000000, SERIAL_8N1, RX_GPIO, TX_GPIO);
  Serial1.setTimeout(20);  // Cap parseFloat() blocking; default 1000ms stalls the radar loop.
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

  // Put device to sleep after 120 seconds after setup
  sleep_gate.sleep_time = millis() + 10000;  //120000
  sleep_gate.wake_flag  = true;

  // ignore device measurements for 5 seconds after startup
  sleep_gate.ignore_time = millis() + 5000;
  sleep_gate.ignore_flag = true;

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
  while (1) {                      // Loop indefinitely

    dnsServer.processNextRequest();  // Process request for ESPUI

    if (sleep_gate.ignore_flag == true) {
      if (millis() >= sleep_gate.ignore_time) {
        sleep_gate.ignore_flag = false;  // Only if 5 seconds passed
      }
    }

    if (radar.is_kph == true) {
      radar.speed = get_speed(true);  // Get speed (KPH) from STM32 via UART
    } else {
      radar.speed = get_speed(false);  // Get speed (MPH) from STM32 via UART
    }

    //Serial.println(radar.speed);  // TESTING

    /* SLEEP - 120 Seconds after startup
     *  Gives time for user to make changes over WiFi
     */

    if (sleep_gate.wake_flag == true) {
      if (millis() >= sleep_gate.sleep_time) {   // Only if 120 seconds passed
        if (digitalRead(ESP_WAKEUP_PIN) == 0) {  // Only if STM not measuring data
          sleep_gate.wake_flag = false;
          Serial.println("Going to sleep 1");  // Go to sleep

          esp_light_sleep_start();
          WiFi.disconnect(true);  // Drop creds so the reconnect path runs fresh after wake.
          WiFi.mode(WIFI_OFF);
        }
      }
    }

    /* SLEEP - 5 seconds of no activity on radar */
    unsigned long currentMillis = millis();

    if (sleep_gate.wake_flag == false) {
      if (radar.speed == 0) {  // Check if speed is 0
        if (currentMillis - previousMillis >= interval) {
          previousMillis = currentMillis;
          Serial.println("Going to sleep 2");

          esp_light_sleep_start();
          WiFi.disconnect(true);
          WiFi.mode(WIFI_OFF);
          previousMillis = millis();
        }
      }
    }

    // Checks if wifi is disconnected and sets connect flag to true. Connection will occur on Core 0
    if (digitalRead(ESP_WAKEUP_PIN) == 1) {  // STM32 has detected motion; bring networking up.
      if (WiFi.status() != WL_CONNECTED) {
        upload.connect_wifi = true;
      }
    }

    if (sleep_gate.ignore_flag == false) {
      if (radar.speed >= radar.min_speed) {

        Serial.println("Min speed triggered");
        Serial.println(radar.min_speed);
        Serial.println(radar.speed);

        delay(100);
        radar.maxSpeed = 0;  // Tracks the max speed during the entire duraton of tracking
        bool collect_data_point = true;
        upload.send_data = false;
        upload.send_photo = false;
        upload.speed_collection_complete = false;  // Don't send data until photo is finished

        while (radar.speed >= radar.min_speed) {  // Capture the cars maximum speed during the entire drive. Then reset back to zero. Send this max speed.
          if (radar.is_kph == true) {
            radar.speed = get_speed(true);  // Get speed (KPH) from STM32 via UART
          } else {
            radar.speed = get_speed(false);  // Get speed (MPH) from STM32 via UART
          }

          if (radar.speed > radar.maxSpeed) {
            radar.maxSpeed = radar.speed;
            Serial.print("New maxSpeed: ");
            Serial.println(radar.maxSpeed);
          }

          if ((radar.maxSpeed >= radar.photo_speed) && (collect_data_point == true)) {  // Only take a photo if one is not already in progress
            Serial.println("Taking photo_speed photo");
            if (takePhoto()) {
              upload.send_data = true;  // Process photo on Core 0
            } else {
              // Previous upload still pending or capture failed; skip this
              // event so we don't trample the framebuffer Core 0 is reading.
              Serial.println("Skipping upload trigger this event");
            }
            collect_data_point = false;  // flag that activates photo only one time
          }

          delay(100);
        }

        Serial.print("MAX maxSpeed: ");
        Serial.println(radar.maxSpeed);

        if (radar.maxSpeed >= radar.photo_speed) {
          upload.send_photo = true;
        }

        upload.speed_collection_complete = true;  // Signal to httpsSend task to send data

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
  while (1) {

    if (upload.send_data == true) {
      sendPhoto();
      upload.send_data = false;
    }

    if (upload.connect_wifi == true) {
      connectWifi();
      upload.connect_wifi = false;
    }

    delay(10);
  }
}
