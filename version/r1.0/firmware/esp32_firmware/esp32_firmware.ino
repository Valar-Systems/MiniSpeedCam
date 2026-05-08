/**
 * MiniSpeedCam - ESP32 Firmware (Hardware revision 1.0)
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
 * Revision 1.0 specifics: this build uses esp_light_sleep_start() to
 * actually enter light sleep when the radar is idle. Later revisions
 * disable that path because USB disconnects on sleep.
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
#include "SPIFFS.h"
#include "Base64.h"

// --- GPIO assignments (board revision 1.0) ---
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

DNSServer dnsServer;  // Captive-DNS used in AP mode so any URL hits the ESPUI portal

TaskHandle_t Task0;  // Handle for the Core 0 task (HTTPS uploads / WiFi reconnect)
TaskHandle_t Task1;  // Handle for the Core 1 task (radar polling / sleep / DNS)

/**
 * Arduino setup() - runs once on boot/reset.
 *
 * Initializes peripherals, restores user settings from NVS, brings the
 * STM32 radar out of reset, joins WiFi (or starts the configuration AP),
 * launches the ESPUI web UI, and finally pins the two work loops to
 * separate cores so radar polling never blocks on HTTPS uploads.
 *
 * Note (r1.0): this revision arms light-sleep wakeup on ESP_WAKEUP_PIN
 * via esp_sleep_enable_ext0_wakeup() so the device can sleep until the
 * STM32 reports movement.
 */
void setup() {
  Serial.begin(115200);

  // GPIO setup
  pinMode(ESP_WAKEUP_PIN, INPUT);
  pinMode(WIFI_RESET_PIN, INPUT);  // Set the wifi reset pin
  pinMode(STM32_RESET_PIN, OUTPUT);
  pinMode(CAMERA_PWDN_PIN, OUTPUT);  // Set the camera powerdown pin
  pinMode(CAMERA_RST_PIN, OUTPUT);   // Set the camera reset pin // Causes crash

  // Light Sleep setup
  esp_sleep_enable_ext0_wakeup(ESP_WAKEUP_PIN, 1);  // Wake up ESP32 when GPIO1 is HIGH //STM will always pull GIO1 high when speeds above 5 mph are detected. Will pull low when speeds

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
  sleep_time = millis() + 10000;  //120000
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
 * Arduino loop() - intentionally minimal.
 *
 * All recurring work happens on taskCore0 / taskCore1, which are pinned
 * to separate cores via xTaskCreatePinnedToCore() in setup(). The debug
 * print of the wake-up pin level here is left over from bring-up.
 */
void loop() {
  // Loop not used. Use tasks intead for dual-core performance
  Serial.println(digitalRead(ESP_WAKEUP_PIN));
  delay(100);
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
 *   4. Manages two sleep paths (post-boot grace, and 5s of no radar);
 *      r1.0 actually calls esp_light_sleep_start() and re-arms the
 *      camera + WiFi reconnect flag on wake.
 *   5. When speed >= min_speed, captures the run's max speed, fires the
 *      camera once at photo_speed, and signals Core 0 to upload.
 *   6. Polls the WiFi-reset button.
 */
void taskCore1(void* parameter) {  // Code for task running on Core 1
  while (1) {                      // Loop indefinitely

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

    Serial.println(speed);  // TESTING
    /* SLEEP - 120 Seconds after startup
     *  Gives time for user to make changes over WiFi
     */

    if (wake_flag == true) {
      if (millis() >= sleep_time) {              // Only if 120 seconds passed
        if (digitalRead(ESP_WAKEUP_PIN) == 0) {  // Only if STM not measuring data
          wake_flag = false;
          Serial.println("Going to sleep 1");  // Go to sleep
          delay(1000);
          esp_light_sleep_start();
          cameraPowerOn();  // power on camera after waking
          Serial.println("WAKING UP 1!!!");
          connect_wifi = true;
          //
        }
      }
    }

    /* SLEEP - 5 seconds of no activity on radar */
    unsigned long currentMillis = millis();

    if (wake_flag == false) {
      if (speed == 0) {  // Check if speed is 0
        if (currentMillis - previousMillis >= interval) {
          previousMillis = currentMillis;      // Save the last time
          Serial.println("Going to sleep 2");  // Go to sleep
          delay(1000);
          esp_light_sleep_start();
          cameraPowerOn();  // power on camera after waking
          Serial.println("WAKING UP 2!!!");
          previousMillis = millis();
          connect_wifi = true;
        }
      }
    }

    if (ignore_flag == false) {
      if (speed >= min_speed) {

        Serial.println("Min speed triggered");
        Serial.println(min_speed);
        Serial.println(speed);

        delay(100);
        maxSpeed = 0;  // Tracks the max speed during the entire duraton of tracking
        bool collect_data_point = true;
        send_data = false;
        send_photo = false;
        speed_collection_complete = false;  // Don't send data until photo is finished

        while (speed >= min_speed) {  // Capture the cars maximum speed during the entire drive. Then reset back to zero. Send this max speed.
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
            takePhoto();
            send_data = true;            // Process photo on Core 0
            collect_data_point = false;  // flag that activates photo only one time
          }

          delay(100);
        }

        Serial.print("MAX maxSpeed: ");
        Serial.println(maxSpeed);

        if (maxSpeed >= photo_speed) {
          send_photo = true;
        }

        send_API = true;
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
 * flags raised by Core 1: send_data triggers an upload (via
 * takeSendPhoto in r1.0), connect_wifi triggers a (re)connection attempt.
 */
void taskCore0(void* parameter) {
  while (1) {

    if (send_data == true) {
      takeSendPhoto();
      send_data = false;
    }

    if (connect_wifi == true) {
      connectWifi();
      connect_wifi = false;
    }

    delay(10);
  }
}
