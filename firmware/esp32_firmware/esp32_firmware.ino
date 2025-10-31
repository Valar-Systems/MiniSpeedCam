/* Libraries
 * Async TCP 3.3.8
 * ESPUI 2.2.4
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

#define STM32_RESET_PIN GPIO_NUM_47
#define RX_GPIO 42
#define TX_GPIO 41
#define ESP_WAKEUP_PIN GPIO_NUM_1  // Only RTC IO are allowed - ESP32 Pin example
#define STM_WAKEUP_PIN GPIO_NUM_2  // Only RTC IO are allowed - ESP32 Pin example
#define WIFI_RESET_PIN GPIO_NUM_21

#define CAMERA_PWDN_PIN GPIO_NUM_45
#define CAMERA_RST_PIN GPIO_NUM_19

Preferences preferences;

#include "variables.h"
#include "camera.h"
#include "radar.h"
#include "api.h"
#include "espui_settings.h"

DNSServer dnsServer;

TaskHandle_t Task0;
TaskHandle_t Task1;

void setup() {
  Serial.begin(115200);

  // GPIO setup
  pinMode(ESP_WAKEUP_PIN, INPUT);
  pinMode(WIFI_RESET_PIN, INPUT);  // Set the wifi reset pin
  pinMode(STM32_RESET_PIN, OUTPUT);
  pinMode(CAMERA_PWDN_PIN, OUTPUT);  // Set the camera powerdown pin
  pinMode(CAMERA_RST_PIN, OUTPUT);   // Set the camera reset pin

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

void loop() {
  // Loop not used. Use tasks intead for dual-core performance
}


unsigned long previousMillis = 0;  // Stores last time LED was updated
const long interval = 5000;        // Interval at which to fall asleep (milliseconds)

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
          //esp_wifi_stop(); Disables Wifi to save battery
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
