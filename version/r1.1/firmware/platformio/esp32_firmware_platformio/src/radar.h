/**
 * radar.h - UART protocol with the STM32 (which drives the CDM324 radar).
 *
 * The STM32 performs the FFT on the amplified CDM324 signal and exposes
 * a tiny ASCII protocol over UART1 (1,000,000 baud, 8N1):
 *   - Send 'm' to request the latest speed in MPH * 10.
 *   - Send 'k' to request the latest speed in KPH * 10.
 *   - The STM32 replies with "<value>,<mag>,<snr>*<CK>\r\n", where <value> is
 *     speed*10, <mag> is the clamped FFT peak magnitude (a proximity proxy,
 *     ~1/r^4; 0 when no target), <snr> is the peak/floor ratio*10 (detection
 *     quality), and <CK> is the XOR of every ASCII char of the
 *     "<value>,<mag>,<snr>" payload (digits and the commas) as two hex chars
 *     (e.g. "298,1024,57*2d"). We verify the checksum and reject any reply that
 *     doesn't match, so UART corruption (frequent on this noisy board) can't
 *     inject a plausible-but-wrong speed. We publish <mag> as g_last_peak_mag
 *     for the proximity gate and <snr> as g_last_peak_snr for per-event
 *     telemetry. The <snr> field is optional: an older STM32 that sends only
 *     "<value>,<mag>" still parses (g_last_peak_snr just stays 0).
 *
 * On reset, the STM32 emits a banner string which we drain here so it
 * doesn't pollute the first speed query.
 */

#include <stdlib.h>  // strtol, atoi
#include <string.h>  // strchr

void issue_cdm324_reset(void);

// Physical ceiling for a believable vehicle speed, in the configured units
// (after the /10 scaling below). RF noise — frequently amplified by nearby
// WiFi transmission — makes the STM32's FFT emit nonsensical spikes; any
// reading beyond this is treated as junk and discarded. 250 comfortably
// covers real road speeds in both MPH and KPH while rejecting noise.
static const float MAX_PLAUSIBLE_SPEED = 250.0f;

// A get_speed() streak this long with no STM reply at all means the radar MCU
// has gone mute (crashed, or sitting in a mid-failed reflash) -- distinct from
// "no car," which still arrives as a checksummed 0-speed line. The power-saver
// sleep paths check stmLinkDead() and keep WiFi up while it holds, so the STM
// can be reflashed over the air instead of the device stranding offline.
static const uint32_t STM_DEAD_STREAK = 30;   // ~3s at the ~100ms poll cadence
static inline bool stmLinkDead(void) { return g_stm_no_reply_streak >= STM_DEAD_STREAK; }

/**
 * Query the STM32 for the most recent speed sample.
 *
 * @param kmh  true = request KPH, false = request MPH.
 * @return     Speed in the requested units. Returns 0 if the STM32 did
 *             not have data ready, or if the reading was implausible
 *             (negative / above MAX_PLAUSIBLE_SPEED) and treated as noise.
 */
float get_speed(bool kmh) {
  // Clear the proximity + SNR readings up front: every early-return below (no
  // data, bad checksum, junk speed) then leaves them at 0, so a stale value can
  // never arm a run or pollute telemetry. Only a fully valid reply sets them.
  g_last_peak_mag = 0;
  g_last_peak_snr = 0;

  // Drain any stale or noise bytes sitting in the RX buffer so we parse the
  // fresh reply rather than leftover garbage (UART line noise from WiFi
  // activity can leave partial/junk bytes here).
  while (Serial1.available() > 0) {
    Serial1.read();
  }

  if (kmh == true) {
    // Query km/h * 10
    Serial1.print('k');
  } else {
    // Query mph * 10
    Serial1.print('m');
  }

  // Read the reply line "<value>,<mag>*<CK>\r\n", bounded to 50ms. We collect a
  // full line (rather than parseFloat) so the checksum can be verified; the
  // original could also race the reply and let parseFloat block on noise.
  char line[24];
  int idx = 0;
  bool got_line = false;
  unsigned long start = millis();
  while ((millis() - start) < 50) {
    if (Serial1.available() > 0) {
      char c = (char)Serial1.read();
      if (c == '\n') {
        got_line = true;
        break;
      }
      if (c != '\r' && idx < (int)sizeof(line) - 1) {
        line[idx++] = c;
      }
    }
  }
  line[idx] = '\0';

  if (!got_line) {
    Serial.println("NO DATA");
    g_stm_no_reply_streak++;  // STM mute -> dead-link signal (not "no target")
    return 0;  // nothing complete arrived; report no motion rather than stale
  }
  g_stm_no_reply_streak = 0;  // a line arrived (even a bad-checksum one) -> link alive

  // Split "<value>*<CK>" and verify the checksum (XOR of the value's ASCII
  // digits). A mismatch means the line was corrupted in transit — reject it.
  // This catches the dangerous case the plausibility ceiling can't: a flipped
  // bit that lands on another in-range value (e.g. 29 -> 79 mph).
  char* star = strchr(line, '*');
  if (star == nullptr) {
    Serial.printf("[RADAR] reply has no checksum: \"%s\"\n", line);
    return 0;
  }
  *star = '\0';  // terminate the value substring; star+1 is the checksum hex
  const char* value_str = line;
  uint8_t rx_ck = (uint8_t)strtol(star + 1, nullptr, 16);
  uint8_t calc_ck = 0;
  for (const char* p = value_str; *p != '\0'; p++) {
    calc_ck ^= (uint8_t)*p;
  }
  if (calc_ck != rx_ck) {
    Serial.printf("[RADAR] checksum fail: \"%s*%s\"\n", value_str, star + 1);
    g_reject_speed++;  // corrupt reply (telemetry: rejection-rate counter)
    return 0;
  }

  // value_str is "<speed>,<mag>": speed * 10 as an ASCII integer (atoi stops at
  // the comma), and the clamped FFT peak magnitude after it (the proximity
  // proxy). Scale the speed back to real units.
  float speed = atoi(value_str) / 10.0f;

  // Reject implausible values: negative or beyond the physical ceiling are
  // noise, not a real vehicle. Returning 0 keeps junk out of the run-trigger
  // and upload paths entirely (g_last_peak_mag stays 0 from the top of fn).
  if (speed < 0.0f || speed > MAX_PLAUSIBLE_SPEED) {
    Serial.printf("[RADAR] rejected junk reading: %.1f\n", speed);
    g_reject_speed++;  // implausible reply (telemetry: rejection-rate counter)
    return 0;
  }

  // Reading is good: publish the magnitude for the proximity gate and the SNR
  // for telemetry. The reply is "<speed>,<mag>,<snr>"; older STM32 firmware that
  // omits a trailing field simply leaves that value at 0 (proximity gate off).
  const char* comma = strchr(value_str, ',');
  if (comma != nullptr) {
    long mag = strtol(comma + 1, nullptr, 10);
    if (mag < 0) mag = 0;
    if (mag > 65535) mag = 65535;
    g_last_peak_mag = (uint16_t)mag;

    const char* comma2 = strchr(comma + 1, ',');
    if (comma2 != nullptr) {
      long snr = strtol(comma2 + 1, nullptr, 10);
      if (snr < 0) snr = 0;
      if (snr > 65535) snr = 65535;
      g_last_peak_snr = (uint16_t)snr;
    }
  }

  return speed;
}

/**
 * Query the STM32 firmware version via the 'v' command.
 *
 * Returns the version string the STM32 reports ("V<ver>\r\n" -> "<ver>"), or
 * "unknown" if it doesn't answer within the window (older STM32 firmware that
 * predates 'v' simply ignores it). Used by the OTA flow to decide whether the
 * STM32 needs reflashing. Call only at boot, before taskCore1 starts polling,
 * so it can't race get_speed() on the shared UART.
 */
String get_stm32_version() {
  while (Serial1.available() > 0) Serial1.read();  // drain stale bytes
  Serial1.print('v');

  char line[24];
  int idx = 0;
  bool got_line = false;
  unsigned long start = millis();
  while ((millis() - start) < 150) {
    if (Serial1.available() > 0) {
      char c = (char)Serial1.read();
      if (c == '\n') { got_line = true; break; }
      if (c != '\r' && idx < (int)sizeof(line) - 1) line[idx++] = c;
    }
  }
  line[idx] = '\0';

  // Expect "V<version>"; anything else (noise, a stray speed reply) -> unknown.
  if (got_line && line[0] == 'V' && idx > 1) {
    return String(line + 1);
  }
  return String("unknown");
}

/**
 * Reset the STM32 and consume its boot banner.
 *
 * Drives STM32_RESET_PIN low for 20ms, then reads bytes off UART1 until
 * a newline (or the buffer fills) so the next get_speed() starts from
 * a clean RX queue. The banner is echoed on Serial for debugging.
 */
void issue_cdm324_reset() {
  bool string_received = false;
  char receive_buffer[50];
  int index = 0;

  // 20ms reset
  digitalWrite(STM32_RESET_PIN, LOW);
  delay(20);
  digitalWrite(STM32_RESET_PIN, HIGH);

  // Read the STM32 boot banner, but never block boot indefinitely: stop once
  // a newline arrives, the buffer is full, or 1s elapses with no response.
  unsigned long start = millis();
  while (string_received == false && (millis() - start) < 1000) {
    if (Serial1.available() > 0) {
      char bla = Serial1.read();
      receive_buffer[index++] = bla;
      if (bla == '\n' || index >= (int)sizeof(receive_buffer) - 1)
        string_received = true;
    }
  }
  receive_buffer[index] = 0;

  Serial.println("Received from the CDM324:");
  Serial.println(receive_buffer);
}