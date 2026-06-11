/*
  Detecting if an SD Card is present
  By: Nathan Seidle
  SparkFun Electronics
  Date: January 13, 2024
  License: MIT. Please see LICENSE.md for more information.

  The socket detect switch is normally open, and closed when a card is inserted.
  The switch is connected to VDD, so when a card is inserted, the switch is connected
  to 3.3V. Therefore, we use an internal pull down.

  This shows how to control the power to the Qwiic connector. This example also shows the 
  Fuel Gauge IC at address 0x36.

  Feel like supporting open source hardware?
  Buy a board from SparkFun!
  SparkFun ESP32-S3 Thing Plus (DEV-24408) https://www.sparkfun.com/products/24408

  Select the following in the Arduino IDE:
  Board: ESP32S3 Dev Module
  USB Mode: Hardware CDC and JTAG
  USB CDC on Boot: Enabled
  Upload Mode: UART0 / Hardware CDC
  PSRAM: QSPI PSRAM
  Port: Select the COM port that the device shows up on under device manager
*/

int pin_sdDetect = 48;

void setup()
{
  Serial.begin(115200);
  while (Serial == false); //Wait for serial monitor to connect before printing anything
  Serial.println("SDIO test");

  pinMode(pin_sdDetect, INPUT_PULLDOWN);
}

void loop()
{
  if (Serial.available()) ESP.restart();

  if (digitalRead(pin_sdDetect) == HIGH)
    Serial.println("Card inserted");
  else
    Serial.println("No SD card");

  delay(500);
}
