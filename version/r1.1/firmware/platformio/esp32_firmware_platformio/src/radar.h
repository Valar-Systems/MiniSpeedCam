/**
 * radar.h - UART protocol with the STM32 (which drives the CDM324 radar).
 *
 * The STM32 performs the FFT on the amplified CDM324 signal and exposes
 * a tiny ASCII protocol over UART1 (1,000,000 baud, 8N1):
 *   - Send 'm' to request the latest speed in MPH * 10.
 *   - Send 'k' to request the latest speed in KPH * 10.
 *   - The STM32 replies with "<value>*<CK>\r\n", where <CK> is the XOR of the
 *     value's ASCII digits as two hex chars (e.g. "298*33"). We verify the
 *     checksum and reject any reply that doesn't match, so UART corruption
 *     (frequent on this noisy board) can't inject a plausible-but-wrong speed.
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

/**
 * Query the STM32 for the most recent speed sample.
 *
 * @param kmh  true = request KPH, false = request MPH.
 * @return     Speed in the requested units. Returns 0 if the STM32 did
 *             not have data ready, or if the reading was implausible
 *             (negative / above MAX_PLAUSIBLE_SPEED) and treated as noise.
 */
float get_speed(bool kmh) {
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

  // Read the reply line "<value>*<CK>\r\n", bounded to 50ms. We collect a full
  // line (rather than parseFloat) so the checksum can be verified; the original
  // could also race the reply and let parseFloat block on noise.
  char line[16];
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
    return 0;  // nothing complete arrived; report no motion rather than stale
  }

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
    return 0;
  }

  // STM32 reports speed * 10 as an ASCII integer; scale back.
  float speed = atoi(value_str) / 10.0f;

  // Reject implausible values: negative or beyond the physical ceiling are
  // noise, not a real vehicle. Returning 0 keeps junk out of the run-trigger
  // and upload paths entirely.
  if (speed < 0.0f || speed > MAX_PLAUSIBLE_SPEED) {
    Serial.printf("[RADAR] rejected junk reading: %.1f\n", speed);
    return 0;
  }

  return speed;
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