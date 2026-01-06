void issue_cdm324_reset(void); 

float get_speed(bool kmh) {
  if (kmh == true) {
    // Query km/h * 10
    Serial1.print('k');
  } else {
    // Query mph * 10
    Serial1.print('m');
  }

  float speed;
  if (Serial1.available() > 0) {
    speed = Serial1.parseFloat();
  } else {
    Serial.println("NO DATA");
  }

  return (speed / 10);
}

void issue_cdm324_reset() {
  bool string_received = false;
  char receive_buffer[50];
  int index = 0;

  // 20ms reset
  digitalWrite(STM32_RESET_PIN, LOW);
  delay(20);
  digitalWrite(STM32_RESET_PIN, HIGH);

  // get string
  while (string_received == false) {
    if (Serial1.available() > 0) {
      char bla = Serial1.read();
      receive_buffer[index++] = bla;
      if (bla == '\n')
        string_received = true;
    }
  }
  receive_buffer[index] = 0;

  Serial.println("Received from the CDM324:");
  Serial.println(receive_buffer);
}