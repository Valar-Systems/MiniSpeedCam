/**
 * radar.h - UART protocol with the STM32 (which drives the CDM324 radar).
 *
 * The STM32 performs the FFT on the amplified CDM324 signal and exposes
 * a tiny ASCII protocol over UART1 (1,000,000 baud, 8N1):
 *   - Send 'm' to request the latest speed in MPH * 10.
 *   - Send 'k' to request the latest speed in KPH * 10.
 *   - The STM32 replies with an ASCII float terminated by newline.
 *
 * On reset, the STM32 emits a banner string which we drain here so it
 * doesn't pollute the first speed query.
 */

void issue_cdm324_reset(void);

/**
 * Query the STM32 for the most recent speed sample.
 *
 * Drains any leftover bytes (a previous reply we never read), issues the
 * unit-specific command, then blocks in parseFloat() up to Serial1's
 * configured timeout (set in setup() via Serial1.setTimeout) waiting for
 * the response. Without the drain the buffer would always lag by one
 * cycle since the original code checked available() before the STM32
 * had a chance to reply.
 *
 * @param kmh  true = request KPH, false = request MPH.
 * @return     Speed in the requested units (one decimal of resolution),
 *             or 0 if the STM32 did not respond before the timeout.
 */
float get_speed(bool kmh) {
  // Drop any stale bytes from a previous unanswered query so parseFloat
  // doesn't pick up data that belongs to an earlier command.
  while (Serial1.available() > 0) {
    Serial1.read();
  }

  Serial1.print(kmh ? 'k' : 'm');

  // STM32 reports speed * 10 as an ASCII float; scale back to a real value.
  return Serial1.parseFloat() / 10.0f;
}

/**
 * Reset the STM32 and consume its boot banner.
 *
 * Drives STM32_RESET_PIN low for 20ms, then reads bytes off UART1 until
 * a newline, the buffer fills, or the timeout elapses. Bounding the wait
 * matters because this runs from setup(), before the task watchdog has
 * any subscribers - a silent STM32 (unprogrammed, dead, or wired wrong)
 * would otherwise wedge the boot indefinitely.
 */
void issue_cdm324_reset() {
  bool string_received = false;
  char receive_buffer[50];
  int index = 0;

  // 20ms reset pulse
  digitalWrite(STM32_RESET_PIN, LOW);
  delay(20);
  digitalWrite(STM32_RESET_PIN, HIGH);

  const unsigned long banner_timeout_ms = 1000;
  const unsigned long start = millis();

  while (!string_received && (millis() - start) < banner_timeout_ms) {
    if (Serial1.available() > 0) {
      char bla = Serial1.read();
      if (index >= (int)sizeof(receive_buffer) - 1) {
        break;  // Buffer full; bail out before we overflow.
      }
      receive_buffer[index++] = bla;
      if (bla == '\n') {
        string_received = true;
      }
    }
  }
  receive_buffer[index] = 0;

  if (string_received) {
    Serial.println("Received from the CDM324:");
    Serial.println(receive_buffer);
  } else {
    Serial.println("Timed out waiting for CDM324 banner; continuing anyway");
  }
}