/*
  Read analog values from the analog pins
  By: Nathan Seidle
  SparkFun Electronics
  Date: January 13, 2024
  License: MIT. Please see LICENSE.md for more information.

  This example shows how to read analog values from the pins marked analog on
  the SparkFun ESP32 S3 Thing Plus

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

int analogPins[] = {10, 14, 15, 16, 17, 18};

void setup()
{
  Serial.begin(115200);
  while (Serial == false); //Wait for serial monitor to connect before printing anything
  Serial.println("Analog reading");

  int spotTest = 0;

  while (1)
  {
    int pinTest = analogPins[spotTest];

    pinMode(pinTest, INPUT);

    Serial.printf("Testing pin %d. Press any key to move to next test.\r\n", pinTest);

    while (Serial.available()) Serial.read();

    while (1)
    {
      int analogValue = analogRead(pinTest);

      Serial.print("Pin = ");
      Serial.print(pinTest);
      Serial.print("\t analogValue = ");
      Serial.println(analogValue);
      delay(50);

      if (Serial.available())
        break;
    }

    spotTest++;
    if (spotTest >= (sizeof(analogPins) / sizeof(int)))
      break;
  }
  Serial.println("Done");

  delay(100);
  while (Serial.available()) Serial.read();

}

void loop()
{
}
