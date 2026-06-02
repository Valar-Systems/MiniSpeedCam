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
 * @param kmh  true = request KPH, false = request MPH.
 * @return     Speed in the requested units. Returns 0 if the STM32 did
 *             not have data ready when polled.
 */
float get_speed(bool kmh) {
  if (kmh == true) {
    // Query km/h * 10
    Serial1.print('k');
  } else {
    // Query mph * 10
    Serial1.print('m');
  }

  float speed = 0;  // default to 0 so a stale/garbage value is never returned when no data is ready
  if (Serial1.available() > 0) {
    speed = Serial1.parseFloat();
  } else {
    Serial.println("NO DATA");
  }

  // STM32 reports speed * 10 as an integer-style ASCII float; scale back to a real value.
  return (speed / 10);
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