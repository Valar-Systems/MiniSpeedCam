/*
  Drive various pins high/low
  By: Nathan Seidle
  SparkFun Electronics
  Date: January 13, 2024
  License: MIT. Please see LICENSE.md for more information.

  This example shows how to toggle the avilable pins on the ESP32 S3 Thing Plus. 

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

//int gpios[] = {43, 44, 8, 9, 13, 11, 12, 10, 14, 15, 16, 17, 18, 21, 7, 6, 5, 4, 2, 1, 42};
int gpios[] = {0}; //STAT LED

void setup()
{
  Serial.begin(115200);
  while (Serial == false); //Wait for serial monitor to connect before printing anything
  Serial.println("GPIO test");

  int spotTest = 0;

  while (1)
  {
    int pinTest = gpios[spotTest];

    pinMode(pinTest, OUTPUT);

    Serial.printf("Testing pin %d. Press any key to move to next test.\r\n", pinTest);
    
    while (Serial.available()) Serial.read();
    
    while (1)
    {
      Serial.printf("Pin %d low\r\n", pinTest);
      digitalWrite(pinTest, LOW);
      delay(2000);
      if (Serial.available())
        break;

      Serial.printf("Pin %d high\r\n", pinTest);
      digitalWrite(pinTest, HIGH);
      delay(2000);
      if (Serial.available())
        break;
    }

    digitalWrite(pinTest, HIGH);
    pinMode(pinTest, INPUT);

    spotTest++;
    if (spotTest >= (sizeof(gpios) / sizeof(int)))
      break;
  }
  Serial.println("Done");

  delay(100);
  while (Serial.available()) Serial.read();
}

void loop()
{

}
