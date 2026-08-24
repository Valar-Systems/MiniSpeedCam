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
 *   - Hosts a self-hosted web config portal (esp_http_server on :80, over
 *     WiFi STA or a captive AP if no credentials are stored) for configuring
 *     WiFi, units (MPH/KPH), minimum tracking speed, and photo trigger speed.
 *   - Implements a two-task split across the dual-core ESP32:
 *       Core 1 (Task1) - radar polling, sleep management, DNS.
 *       Core 0 (Task0) - HTTPS uploads and (re)connecting to WiFi.
 *
 * PlatformIO port of esp32_firmware/esp32_firmware.ino. The module
 * headers below contain their definitions and are compiled as a single
 * translation unit (exactly as the Arduino IDE concatenated the .ino
 * tabs), so behaviour is identical to the original sketch.
 *
 * Required libraries (see platformio.ini lib_deps):
 *   - ArduinoJson (config-portal /api/state + OTA manifest parsing)
 * The web config portal and aiming-stream server both use the IDF-native
 * esp_http_server (no ESPAsyncWebServer / AsyncTCP).
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <esp_camera.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>

#include "soc/soc.h"           // Disable brownout problems
#include "soc/rtc_cntl_reg.h"  // Disable brownout problems
#include "driver/rtc_io.h"
#include "SPIFFS.h"
#include "esp_core_dump.h"     // read the previous crash's saved backtrace on boot
#include "esp_system.h"        // esp_reset_reason()

// --- GPIO assignments (board revision 1.1) ---
#define STM32_RESET_PIN GPIO_NUM_47  // Drives STM32 NRST low to reset the radar MCU
#define STM32_BOOT0_PIN GPIO_NUM_14  // Drives STM32 BOOT0 (HIGH = ROM bootloader) for ESP-driven flashing
#define RX_GPIO 42                   // UART1 RX from STM32 (speed reports)
#define TX_GPIO 41                   // UART1 TX to STM32 (speed query commands)
#define ESP_WAKEUP_PIN GPIO_NUM_1    // STM32 pulls HIGH when motion >= ~5mph is detected
#define STM_WAKEUP_PIN GPIO_NUM_2    // (was the reserved STM_WAKEUP line; now the radar-blank line below)
#define RADAR_BLANK_PIN STM_WAKEUP_PIN  // ESP -> STM32 PA4: HIGH during WiFi bursts so the STM32 discards rail-corrupted FFT frames
#define WIFI_RESET_PIN GPIO_NUM_21   // Active-low button: hold 3s to clear stored WiFi creds

#define CAMERA_PWDN_PIN GPIO_NUM_45  // OV2640 power-down (active high)
#define CAMERA_RST_PIN GPIO_NUM_3    // OV2640 reset (active low)

Preferences preferences;  // NVS-backed key/value store for WiFi creds and user settings

// Post-boot window during which WiFi/portal stays up so the user can reach the
// config page after a reboot; Power-Saver then drops WiFi when idle. (Was
// mistakenly shipped as a 10s debug value -- the trailing "//120000" gave it
// away -- which left almost no window to reach the portal and killed the first
// OTA check before it could run.)
static const unsigned long POST_BOOT_WIFI_GRACE_MS = 120000;

#include "variables.h"
#include "events.h"          // recent-detection ring served by GET /api/events (LAN companions)
#include "last_photo.h"      // retained last-capture JPEG for the config portal (GET /api/last.jpg)
#include "diagnostics.h"
#include "camera.h"
#include "camera_stream.h"
#include "radar.h"
#include "api.h"
#include "stm32boot.h"
#include "ota.h"
#include "config_portal.h"

DNSServer dnsServer;  // Captive-DNS used in AP mode so any URL hits the config portal

TaskHandle_t Task0;  // Handle for the Core 0 task (HTTPS uploads / WiFi reconnect)
TaskHandle_t Task1;  // Handle for the Core 1 task (radar polling / sleep / DNS)

// Forward declarations for the dual-core task functions (defined below).
void taskCore0(void* parameter);
void taskCore1(void* parameter);

// The aiming MJPEG stream server lives in its own translation unit
// (camera_stream.cpp). Its desired state is the shared stream_active/
// stream_deadline flags (set by POST /api/stream), and the config page reports
// the live stream URL/state by polling GET /api/state -- so the stream module
// needs no start/stop callbacks wired here anymore.

// --- Crash diagnostics ------------------------------------------------------
// The panic handler saves a core dump to the flash 'coredump' partition on every
// crash, but the native USB-CDC console drops off during the crash so the live
// panic is never seen. Print the saved dump once on the next boot (USB-CDC is
// back by then), then erase it. Decode the printed addresses with:
//   xtensa-esp32s3-elf-addr2line -pfiaC -e .pio/build/esp32-s3/firmware.elf <addr ...>
static void reportLastCrash() {
  if (esp_core_dump_image_check() != ESP_OK) {
    return;  // no valid core dump in flash -> last boot was clean
  }
  Serial.println("!!! ===== PREVIOUS CRASH: core dump found in flash ===== !!!");
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
  char reason[200];
  if (esp_core_dump_get_panic_reason(reason, sizeof(reason)) == ESP_OK) {
    Serial.printf("  reason   : %s\n", reason);
  }
  esp_core_dump_summary_t* s = (esp_core_dump_summary_t*) malloc(sizeof(*s));
  if (s != nullptr && esp_core_dump_get_summary(s) == ESP_OK) {
    Serial.printf("  task     : %s\n", s->exc_task);
    Serial.printf("  PC       : 0x%08x  (exccause %u, fault addr 0x%08x)\n",
                  (unsigned)s->exc_pc, (unsigned)s->ex_info.exc_cause,
                  (unsigned)s->ex_info.exc_vaddr);
    Serial.print("  backtrace:");
    for (uint32_t i = 0; i < s->exc_bt_info.depth; i++) {
      Serial.printf(" 0x%08x", (unsigned)s->exc_bt_info.bt[i]);
    }
    Serial.println(s->exc_bt_info.corrupted ? "  (stack corrupted)" : "");
  }
  free(s);
#endif
  Serial.println("  decode   : xtensa-esp32s3-elf-addr2line -pfiaC -e .pio/build/esp32-s3/firmware.elf <PC + backtrace>");
  esp_core_dump_image_erase();  // clear so it prints once per crash, not every boot
  Serial.println("!!! ==================================================== !!!");
}

/**
 * Arduino setup() - runs once on boot/reset.
 *
 * Initializes peripherals, restores user settings from NVS, brings the
 * STM32 radar out of reset, joins WiFi (or starts the configuration AP),
 * launches the web config portal, and finally pins the two work loops to
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
  pinMode(STM32_BOOT0_PIN, OUTPUT);
  digitalWrite(STM32_BOOT0_PIN, LOW);  // BOOT0 low = STM32 boots from flash (normal run)
  pinMode(RADAR_BLANK_PIN, OUTPUT);
  digitalWrite(RADAR_BLANK_PIN, LOW);  // default: not blanking the radar
  pinMode(CAMERA_PWDN_PIN, OUTPUT);  // Set the camera powerdown pin
  pinMode(CAMERA_RST_PIN, OUTPUT);   // Set the camera reset pin // Causes crash

  // Light Sleep setup
  //esp_sleep_enable_ext0_wakeup(ESP_WAKEUP_PIN, 1);  // Wake up ESP32 when GPIO1 is HIGH //STM will always pull GIO1 high when speeds above 5 mph are detected. Will pull low when speeds

  // Setup Camera. esp_camera_init() can fail transiently on a marginal power
  // rail, so power-cycle and retry a few times rather than booting blind with
  // a dead sensor (which would silently upload empty photos).
  cameraPowerOn();
  bool camera_ok = false;
  for (int attempt = 1; attempt <= 3; attempt++) {
    if (cameraSetup() != 0) { camera_ok = true; break; }
    // The old loop tested cameraSetup() in the while-condition, so on all-fail it
    // ran a 4th init while reporting "3 attempts", and -- worse -- a camera that
    // finally came up on the last try was logged as FAILED. Count each of exactly
    // three attempts here and report success/failure from the actual result.
    Serial.printf("[CAM] init attempt %d/3 failed\n", attempt);
    esp_camera_deinit();  // release any partial init before re-attempting
    delay(250);
    cameraPowerOn();
  }
  if (camera_ok) {
    Serial.println("[CAM] ready");
  } else {
    Serial.println("[CAM] FAILED after 3 attempts; photo capture disabled");
  }

  SPIFFS.begin(true);
  delay(100);

  // load saved variables
  preferences.begin("local", false);
  diagnosticsLogBoot();  // record reset reason + bump persisted boot counter for the Status tab
  otaHealthBootCheck();  // if a just-applied ESP image is crash-looping, revert to the previous slot now
  ssid = preferences.getString("ssid", "NOT_SET");
  password = preferences.getString("pass", "NOT_SET");
  api_base_url = preferences.getString("api_base", API_BASE_URL);  // data destination; user-editable on the Wifi tab (default from config.h)
  rebuildServerEndpoints();  // build the register/capture/camera URLs from api_base_url
  Serial.printf("[API] base URL: %s\n", api_base_url.c_str());
  loadOrCreateIdentity();  // device_token + claim_code: minted once on first boot, reused forever
  min_speed = preferences.getInt("min_speed", 3);             // The minimum speed (MPH) that the tracker should track any vehicle and upload data
  if (min_speed < 1) min_speed = 1;  // 0 would make the run loop `while (speed >= min_speed)` never end (get_speed never returns <0)
  photo_speed = preferences.getInt("photo_speed", 10);        // Cars speed (MPH) when photo should be taken
  min_signal = preferences.getInt("min_signal", 0);           // Min radar echo strength to arm a run (0 = off; proximity gate, see variables.h)
  photo_signal = preferences.getInt("photo_signal", 0);       // Shared fire threshold (direction-aware proximity timing; 0 = off, capture at photo_speed)
  photo_signal_front = preferences.getInt("ps_front", 0);     // FRONT (oncoming) fire threshold; 0 = inherit photo_signal (raise to catch oncoming closer)
  photo_signal_rear = preferences.getInt("ps_rear", 0);       // REAR (receding) fire threshold; 0 = inherit photo_signal (lower to catch receding farther)
  is_kph = preferences.getBool("is_kph", 0);                  // Cars speed (MPH) when photo should be taken
  power_saver = preferences.getBool("power_saver", true);     // true = drop WiFi during idle (default); false = never sleep
  // Installation cosine correction (see variables.h). The !(a && b) form also
  // rejects NaN from a corrupted NVS float, since NaN comparisons are false.
  speed_correction = preferences.getFloat("speed_corr", 1.0f);
  if (!(speed_correction >= 1.0f && speed_correction <= 1.3f)) speed_correction = 1.0f;

  // Connect CDM324 sensor
  Serial.println("Connecting CDM324");
  Serial1.begin(1000000, SERIAL_8N1, RX_GPIO, TX_GPIO);
  Serial.setDebugOutput(false);

  // Reset CDM324
  digitalWrite(STM32_RESET_PIN, LOW);
  issue_cdm324_reset();

  // Read the STM32 firmware version now (before taskCore1 starts polling, so it
  // can't race get_speed() on the shared UART). Retry a few times in case the
  // STM32 is mid-FFT when the first query lands; stays "unknown" on old firmware.
  strlcpy(stm_fw_version, get_stm32_version().c_str(), sizeof stm_fw_version);
  for (int i = 0; i < 3 && strcmp(stm_fw_version, "unknown") == 0; i++) {
    delay(50);
    strlcpy(stm_fw_version, get_stm32_version().c_str(), sizeof stm_fw_version);
  }
  Serial.printf("[FW] esp=%s stm=%s\n", FW_VERSION, stm_fw_version);
  // Connect to WiFi or create Access Point
  connectWifiAP();
  if (WiFi.getMode() == WIFI_STA) {
    Serial.printf("[WIFI] STA ip=%s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.printf("[WIFI] AP ip=%s\n", WiFi.softAPIP().toString().c_str());
  }

  // Start the self-hosted config portal (esp_http_server on :80). Replaces ESPUI.
  load_config_portal();

  // Send local IP address to API if connected to internet
  sendLocalIP();

  // Keep WiFi/portal up for the post-boot grace window so the user can reach the
  // config page after a reboot; Power-Saver then drops WiFi once idle.
  sleep_time = millis() + POST_BOOT_WIFI_GRACE_MS;
  wake_flag = true;

  // ignore device measurements for 5 seconds after startup
  ignore_time = millis() + 5000;
  ignore_flag = true;

  // Queue used to hand finished tracking runs from Core 1 to Core 0. Depth 2
  // lets a second event arrive while one is still uploading; deeper backlogs
  // are dropped (with a log) in taskCore1.
  uploadQueue = xQueueCreate(2, sizeof(UploadRequest));

  // Crash diagnostics are printed HERE, near the end of setup, on purpose: on
  // native USB-CDC the host monitor only reattaches a few seconds into boot, so
  // anything printed at the very top is missed. By now it's reliably connected.
  // This reveals why the previous boot ended -- BROWNOUT (5V rail dip, leaves no
  // core dump) vs PANIC/TASK_WDT (code fault, with a backtrace) vs a clean reset.
  esp_reset_reason_t last_reset = esp_reset_reason();
  Serial.printf("[DIAG] last reset: %s (code %d)\n", resetReasonStr(last_reset), (int)last_reset);
  reportLastCrash();

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
  // All recurring work runs in taskCore0/taskCore1 (pinned in setup()); this
  // Arduino loopTask has nothing to do. Park it rather than spin: an empty loop()
  // leaves loopTask permanently READY at priority 1 on core 1 -- the SAME core and
  // priority as taskCore1 -- so the two round-robin and the radar task gets only
  // ~half the core for no benefit. Blocking yields the core to taskCore1 and lets
  // the idle task run (lower power). Kept alive (not vTaskDelete) so any Arduino
  // serialEvent plumbing still ticks.
  vTaskDelay(pdMS_TO_TICKS(1000));
}

unsigned long previousMillis = 0;  // Timestamp of last idle-sleep tick
const long interval = 5000;        // Idle window (ms) of zero-speed before sleeping

// A run only starts after this many consecutive readings >= min_speed. A lone
// noise spike that slips under the plausibility ceiling (see get_speed) lasts
// a single sample, so requiring 2 in a row rejects glitches while a real
// vehicle (many samples) still triggers immediately.
const int SPEED_CONFIRM_COUNT = 2;

// Direction-aware photo timing fallback: if a car never cleanly crosses
// photo_signal during its run, still fire once magnitude has fallen to this
// percent of the run's peak -- a best-effort "near closest approach" shot.
const int PHOTO_PEAK_DROP_PCT = 70;

// Failsafe cap on a single tracking run. A real vehicle clears the beam in a few
// seconds; a run that lasts this long is sustained noise (a vibrating reflector
// reads as a steady ~20mph) or misconfiguration. Finalize what was measured and
// re-arm rather than let the inner loop wedge taskCore1 -- a wedged loop stops
// servicing the WiFi-reset button, the aiming stream, captive-DNS, and OTA.
const unsigned long MAX_RUN_MS = 60000;

/**
 * Core 1 task: radar polling, sleep policy, and captive-DNS servicing.
 *
 * Each iteration:
 *   1. Pumps the captive-DNS server (inert today: dnsServer.start() is never
 *      called -- see variables.h -- so this is a no-op until it is wired up).
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

    dnsServer.processNextRequest();  // captive-DNS pump; no-op until dnsServer.start() is wired (see variables.h)

    streamService();  // enforce the aiming-stream 5-minute auto-off timeout

    // While the aiming stream is up, skip all measurement work below: the radar
    // reads are meaningless (and WiFi-TX-corrupted) during aiming, and keeping
    // taskCore1 quiet here frees the radio/CPU for a smooth stream. streamBusy()
    // keeps us off the camera until the streaming task has fully exited and
    // restored the sensor to UXGA, so a speed photo can't race the frame-size
    // switch. The short yield replaces get_speed()'s usual loop pacing.
    if (stream_active || streamBusy()) {
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    // While Core 0 is reflashing the STM32 it owns UART1 for the bootloader, so
    // taskCore1 must not poll the radar over the same UART until it's done.
    if (stm_flash_busy) {
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

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

    // No server-side display push here: the config page pulls live readouts
    // (speed, signal, heap, ...) by polling GET /api/state ~1 Hz. This is what
    // removing ESPUI bought us -- the old per-loop WebSocket broadcast over
    // AsyncTCP is exactly what blocked this task and froze the device (~30s:
    // radar, serial, stream all stalled until TCP timed out) under stream load.

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
      if (millis() >= sleep_time) {              // Only if the grace window elapsed
        // Don't drop WiFi out from under an in-flight upload/OTA (coreNetBusy),
        // and don't drop it while the radar link looks dead (stmLinkDead) -- keep
        // the portal/OTA reachable so the STM can be reflashed instead of the
        // device stranding offline. In either case push the deadline out so the
        // grace window simply restarts once Core 0 goes quiet / the radar is back.
        if (coreNetBusy() || stmLinkDead()) {
          sleep_time = millis() + 5000;
        } else if (digitalRead(ESP_WAKEUP_PIN) == 0) {  // Only if STM not measuring data
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
      if (coreNetBusy() || stmLinkDead()) {
        // In-flight upload/OTA, or a dead radar link: hold WiFi up and restart
        // the 5s idle window so we don't tear the radio down mid-transfer (the
        // transfer itself blanks the radar, forcing speed==0 -- the very thing
        // that would otherwise arm this timer).
        previousMillis = currentMillis;
      } else if (speed == 0) {  // Check if speed is 0
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
      // ...but not while we're in config-AP fallback: chasing an unreachable
      // STA there would tear the portal down (handled in connectWifi() too).
      if (!ap_fallback_mode && ssid != "NOT_SET" && !ssid.isEmpty() && WiFi.status() != WL_CONNECTED) {
        connect_wifi = true;
      }
      // Power on camera here? Need to check if it's powered down first

    }

    // Pause speed tracking while the aiming stream is up: the device is being
    // mounted (not measuring), and the stream task owns the camera framebuffer.
    if (ignore_flag == false && !stream_active) {
      // A sample qualifies to arm a run only if it is both fast enough AND
      // close enough: speed >= min_speed and the radar echo magnitude >=
      // min_signal (the proximity gate -- distant cross-traffic returns a weak
      // peak even at speed, so it never qualifies). min_signal == 0 disables
      // the gate (g_last_peak_mag >= 0 is always true), preserving prior
      // behavior until the user calibrates it.
      const bool qualifies =
          (speed >= min_speed) && ((int)g_last_peak_mag >= min_signal);

      // Telemetry: a sample fast enough to arm but too weak (distant cross-
      // traffic the proximity gate dropped). Counts the rejection rate so the
      // gate can be judged remotely. Only meaningful when the gate is enabled.
      if (min_signal > 0 && speed >= min_speed && (int)g_last_peak_mag < min_signal) {
        g_reject_proximity++;
      }

      // Debounce: count consecutive qualifying readings; a single glitch
      // resets the count, so it can never reach SPEED_CONFIRM_COUNT.
      static int speedConfirmCount = 0;
      if (qualifies) {
        speedConfirmCount++;
      } else {
        speedConfirmCount = 0;
      }

      if (qualifies && speedConfirmCount >= SPEED_CONFIRM_COUNT) {
        speedConfirmCount = 0;  // consumed; the run loop below tracks the pass

        Serial.printf("[RUN] start: speed=%d >= min_speed=%d, mag=%u >= min_signal=%d (confirmed)\n",
                      speed, min_speed, (unsigned)g_last_peak_mag, min_signal);

        g_run_active = true;  // tell Core 0's auto-update service a pass is in progress (don't reboot/seize UART)
        delay(100);
        maxSpeed = 0;               // Tracks the max speed over the entire pass
        bool photo_taken = false;   // ensures we capture at most one frame per run
        uint8_t* photo_buf = nullptr;  // ps_malloc'd JPEG copy handed to Core 0 (decoupled from the camera fb pool)
        size_t   photo_len = 0;

        // --- Per-event telemetry accumulators (uploaded with the run) ------
        // Seeded from the arming sample so it's counted; updated each loop.
        const unsigned long run_start = millis();
        uint32_t speed_sum = (speed > 0) ? (uint32_t)speed : 0;  // for mean speed
        uint16_t sample_count = (speed > 0) ? 1 : 0;             // valid frames in the run
        uint16_t peak_snr = g_last_peak_snr;                     // strongest SNR seen
        int direction = 0;                                       // 0 unk, 1 approaching, 2 receding
        // Echo-energy centroid across the run: Sigma(echo_index * mag) / Sigma(mag)
        // over every valid echo locates where the strongest reflections sit in
        // time. Magnitude rises for an oncoming car and falls for a receding one,
        // so energy weighted toward the END of the run => still closing
        // (approaching), toward the START => already leaving (receding), centered
        // => symmetric pass (ambiguous). Uses all samples, so a single noisy
        // reading can't flip it the way a first-vs-last compare could. Drives the
        // direction fallback below and the mag_trend telemetry field. Seeded with
        // the arming echo at index 0.
        uint64_t mag_idx_sum = 0;                                // Sigma(echo_index * mag)
        uint32_t mag_wt_sum  = g_last_peak_mag;                  // Sigma(mag)
        uint16_t echo_n      = (g_last_peak_mag > 0) ? 1 : 0;    // valid echoes counted (mag > 0)

        // --- Direction-aware photo timing (proximity, two thresholds) -------
        // With the unit aimed down the road the echo magnitude (g_last_peak_mag,
        // ~1/r^4) RISES for an oncoming car and FALLS for a receding one. We fire
        // the FRONT shot when a rising echo crosses UP through thr_front, and the
        // REAR shot when a falling echo crosses DOWN through thr_rear. Two
        // thresholds because a car's front reflects far stronger than its rear
        // (so one value frames them at different distances) AND the shots want
        // different framing: RAISE thr_front to catch oncoming closer, LOWER
        // thr_rear to catch receding farther out. Each inherits the shared
        // photo_signal when left at 0, so the old one-knob config still works;
        // both 0 keeps the legacy "first frame past photo_speed" behavior. Keep
        // both >= Min Signal (the arming gate), and Min Signal below thr_front so
        // an oncoming car arms before it crosses (else only the rear/peak fires).
        const int thr_front = (photo_signal_front > 0) ? photo_signal_front : photo_signal;
        const int thr_rear  = (photo_signal_rear  > 0) ? photo_signal_rear  : photo_signal;
        const bool prox_enabled = (thr_front > 0) || (thr_rear > 0);
        bool prev_above_front = (thr_front > 0) && ((int)g_last_peak_mag >= thr_front);
        bool prev_above_rear  = (thr_rear  > 0) && ((int)g_last_peak_mag >= thr_rear);
        uint16_t peak_mag = g_last_peak_mag;
        const char* plate = "legacy";

        while (speed >= min_speed) {  // Track the car's maximum speed over the whole pass.
          // Failsafe: a genuine pass is over in seconds. If we're still here after
          // MAX_RUN_MS the input is sustained noise or a bad config -- finalize
          // what we have and re-arm rather than let this loop wedge the task.
          if (millis() - run_start > MAX_RUN_MS) {
            Serial.println("[RUN] max duration exceeded (noise/misconfig?); finalizing");
            break;
          }

          // Preempt the pass if the aiming stream comes up or an STM flash begins
          // mid-run. capturePhoto() below shares the camera's 2-deep fb pool with
          // the streaming task, and get_speed() above shares UART1 with the STM
          // bootloader; running either concurrently races a resource this loop
          // assumes it owns. Finalize what we have and re-arm instead. Clearing
          // g_run_active at the end of the pass also releases the OTA installer's
          // park-wait, so a pending flash proceeds the moment we step aside.
          if (stream_active || streamBusy() || stm_flash_busy) {
            Serial.println("[RUN] preempted by stream/flash; finalizing");
            break;
          }

          speed = get_speed(is_kph);  // Get speed (KPH or MPH per setting) from STM32 via UART
          // (live readout is served on demand via GET /api/state; nothing to push here)

          if (speed > maxSpeed) {
            maxSpeed = speed;
            Serial.print("New maxSpeed: ");
            Serial.println(maxSpeed);
          }

          // Run-profile telemetry: accumulate valid samples for the mean speed.
          if (speed > 0) {
            speed_sum += (uint32_t)speed;
            sample_count++;
          }

          // A 0 magnitude is "no valid target this sample" -- the STM32 had no
          // fresh FFT, or the reply failed the checksum (both common on this
          // noisy shared-rail board; see INTERFACE.md / get_speed()). It is NOT
          // "the echo fell to zero," so it must not look like a DOWN-crossing:
          // that would fire a bogus REAR-plate shot (and latch photo_taken) on a
          // single dropped or corrupted reply. So act only on samples that carry
          // a real echo and hold the crossing state across a dropout. Legacy mode
          // (prox disabled) ignores magnitude and is unaffected.
          if (prox_enabled && g_last_peak_mag == 0) {
            // dropped/noisy sample: no proximity info -- hold crossing state
          } else {
            if (g_last_peak_mag > peak_mag) peak_mag = g_last_peak_mag;
            if (g_last_peak_snr > peak_snr) peak_snr = g_last_peak_snr;
            if (g_last_peak_mag > 0) {       // valid echo: fold into the energy centroid
              mag_idx_sum += (uint64_t)echo_n * g_last_peak_mag;  // index = current echo_n
              mag_wt_sum  += g_last_peak_mag;
              echo_n++;
            }
            const bool now_above_front = (thr_front > 0) && ((int)g_last_peak_mag >= thr_front);
            const bool now_above_rear  = (thr_rear  > 0) && ((int)g_last_peak_mag >= thr_rear);

            // FRONT fires as a rising echo crosses UP through thr_front (oncoming);
            // REAR fires as a falling echo crosses DOWN through thr_rear (receding).
            const bool cross_up_front  = now_above_front && !prev_above_front;
            const bool cross_down_rear = !now_above_rear && prev_above_rear;

            // Either crossing also reveals direction, independent of whether a
            // photo fires -- recorded once per run for telemetry.
            if (direction == 0) {
              if (cross_up_front) direction = 1;
              else if (cross_down_rear) direction = 2;
            }

            if ((maxSpeed >= photo_speed) && !photo_taken) {  // confirmed speeder, capture once
              bool fire = false;
              if (!prox_enabled) {
                fire = true;  // legacy: first frame past photo_speed
              } else if (cross_up_front) {
                fire = true; plate = "FRONT (oncoming)"; direction = 1;
              } else if (cross_down_rear) {
                fire = true; plate = "REAR (receding)"; direction = 2;
              } else if (peak_mag > 0 &&
                         (int)g_last_peak_mag < (int)((peak_mag * PHOTO_PEAK_DROP_PCT) / 100)) {
                // Past closest approach without a clean crossing: best-effort shot.
                fire = true; plate = "PEAK (fallback)";
                if (direction == 0) direction = 2;  // well past the peak => receding
              }
              if (fire) {
                // Re-check preemption right before touching the camera. The
                // top-of-loop guard can't see a stream that came up later in
                // THIS iteration, and capturePhoto() shares the camera's 2-deep
                // fb pool (and the sensor's framesize) with the aiming-stream
                // task. If a stream/flash is now up, bail: the pass finalizes
                // speed-only rather than race the stream on the sensor.
                if (stream_active || streamBusy() || stm_flash_busy) break;
                Serial.printf("[PHOTO] capture: plate=%s speed=%d max=%d mag=%u peak=%u thr_front=%d thr_rear=%d\n",
                              plate, speed, maxSpeed, (unsigned)g_last_peak_mag,
                              (unsigned)peak_mag, thr_front, thr_rear);
                camera_fb_t* fb = capturePhoto();
                if (fb != nullptr) {
                  // Copy the JPEG into PSRAM and release the camera framebuffer
                  // right away, so the driver's 2-deep fb pool is never held
                  // across the (slow, seconds-long) upload -- otherwise a later
                  // capturePhoto() here (or the aiming stream) could block the
                  // radar task waiting for a free buffer.
                  photo_buf = (uint8_t*)ps_malloc(fb->len);
                  if (photo_buf != nullptr) {
                    memcpy(photo_buf, fb->buf, fb->len);
                    photo_len = fb->len;
                  } else {
                    Serial.println("[PHOTO] ps_malloc failed; uploading speed-only");
                  }
                  esp_camera_fb_return(fb);
                }
                photo_taken = true;
              }
            }
            prev_above_front = now_above_front;
            prev_above_rear  = now_above_rear;
          }

          delay(100);
        }

        // Echo-energy centroid -> normalized trend (0-100, 50 = symmetric pass).
        // Computed every run for telemetry; also serves as the direction fallback
        // when proximity timing isn't configured and no crossing was observed.
        // Energy weighted toward the END (>0.60) means the echo was still rising
        // -> approaching; toward the START (<0.40) means it was falling ->
        // receding. The 0.40-0.60 dead zone (a symmetric pass) is genuinely
        // ambiguous from magnitude alone, so leave it unknown rather than guess.
        // Need >=3 echoes for the centroid to mean anything.
        uint8_t mag_trend = 255;  // 0-100 normalized centroid; 255 = too few echoes
        if (echo_n >= 3 && mag_wt_sum > 0) {
          double pos  = (double)mag_idx_sum / (double)mag_wt_sum;  // 0 .. echo_n-1
          double frac = pos / (double)(echo_n - 1);                // 0 .. 1
          if (frac < 0.0) frac = 0.0; else if (frac > 1.0) frac = 1.0;
          mag_trend = (uint8_t)(frac * 100.0 + 0.5);
          if (direction == 0) {
            if (frac >= 0.60) direction = 1;       // energy late  -> rising  -> approaching
            else if (frac <= 0.40) direction = 2;  // energy early -> falling -> receding
          }
        }

        // The run has ended, so maxSpeed is final. Hand it to Core 0 as a
        // single upload request. A photo is attached only if a capture
        // actually succeeded; otherwise it uploads speed-only.
        UploadRequest req;
        req.speed_actual = maxSpeed;
        req.has_photo = (photo_buf != nullptr);
        req.photo_buf = photo_buf;
        req.photo_len = photo_len;
        req.direction = (uint8_t)direction;
        req.mag_trend = mag_trend;
        req.peak_mag = peak_mag;
        req.peak_snr = peak_snr;
        req.frame_count = sample_count;
        req.duration_ms = millis() - run_start;
        req.mean_speed_x10 =
            sample_count ? (uint16_t)((speed_sum * 10) / sample_count) : 0;
        Serial.printf("[RUN] end: maxSpeed=%d dir=%u trend=%u meanx10=%u frames=%u dur=%lums peak_mag=%u peak_snr=%u has_photo=%d -> enqueue\n",
                      maxSpeed, (unsigned)req.direction, (unsigned)req.mag_trend,
                      (unsigned)req.mean_speed_x10, (unsigned)req.frame_count,
                      (unsigned long)req.duration_ms, (unsigned)req.peak_mag,
                      (unsigned)req.peak_snr, (int)req.has_photo);
        // A pass can reach here with maxSpeed still 0: the preemption break
        // above fired on the very first iteration (before get_speed() updated
        // maxSpeed), or the first sample simply dropped to 0. That is not a real
        // detection, so don't push a phantom 0 mph event into the local ring or
        // up to the cloud -- just release the (normally absent) photo buffer.
        if (maxSpeed > 0) {
          // Record into the local ring served by GET /api/events, so a LAN companion
          // (e.g. the Blipscope "Speedscope" edition) can show recent speeds without the cloud.
          uint32_t ev_seq = eventsRecord(req.speed_actual, req.direction, req.peak_mag);
          // Retain this pass's photo (if it took one) for the config portal's "Last capture"
          // card. lastPhotoStage() makes its own PSRAM copy, so the buffer handed to Core 0
          // via the queue below is still owned/freed by Core 0 as before. Tagged with ev_seq
          // so the portal can tell whether the newest /api/events pass actually has an image.
          if (photo_buf != nullptr) {
            lastPhotoStage(photo_buf, photo_len, ev_seq, req.speed_actual, is_kph, req.direction);
          }
          if (xQueueSend(uploadQueue, &req, 0) != pdTRUE) {
            Serial.println("[RUN] upload queue FULL, dropping event");
            if (photo_buf != nullptr) {
              free(photo_buf);  // avoid leaking the PSRAM JPEG copy
            }
          }
        } else {
          Serial.println("[RUN] no valid sample (preempted/dropped); event discarded");
          if (photo_buf != nullptr) free(photo_buf);  // never leak, even on the discard path
        }

        g_run_active = false;  // pass over; Core 0's auto-update service may act again
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
  // Periodic telemetry heartbeat. sendLocalIP() carries the device-health
  // snapshot (RSSI, heap + low-water mark, uptime, reset reason, rejection
  // counters), so it must be re-sent on an interval -- the once-at-boot call in
  // setup() reports those runtime fields while they're still ~0. It self-guards
  // on STA + connected, so it's a no-op in AP/config mode or while asleep.
  const unsigned long HEARTBEAT_INTERVAL_MS = 10UL * 60UL * 1000UL;  // 10 min: fewer WiFi-TX bursts -> less battery drain (and radar interference)
  unsigned long lastHeartbeat = millis();

  while (1) {

    UploadRequest req;
    if (xQueueReceive(uploadQueue, &req, pdMS_TO_TICKS(200)) == pdTRUE) {
      Serial.printf("[UPLOAD] dequeued event: speed=%d has_photo=%d\n", req.speed_actual, (int)req.has_photo);
      sendUpload(req);
      if (req.photo_buf != nullptr) {
        free(req.photo_buf);  // release the PSRAM JPEG copy Core 1 handed us
      }
    }

    if (connect_wifi == true) {
      Serial.println("[WIFI] reconnect requested");
      connectWifi();
      connect_wifi = false;
    }

    if (millis() - lastHeartbeat >= HEARTBEAT_INTERVAL_MS) {
      lastHeartbeat = millis();
      sendLocalIP();  // telemetry heartbeat (no-op unless STA + connected)
    }

    serviceRegistration();  // trust-on-first-use device registration (retries w/ backoff until 200)
    otaHealthService();  // commit-or-revert a freshly-applied ESP image once it proves healthy
    otaAutoService();    // poll GitHub on a timer and auto-apply newer firmware while idle
  }
}
