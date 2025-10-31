/*
  Enable Qwiic Power Control
  By: Nathan Seidle
  SparkFun Electronics
  Date: January 13, 2024
  License: MIT. Please see LICENSE.md for more information.

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

#include <Wire.h>

int pin_qwiicPower = 45; //Thing Plus S3 45 is connected to the v-reg that controls the Qwiic power

void setup()
{
  Serial.begin(115200);
  while (Serial == false); //Wait for serial monitor to connect before printing anything

  Serial.println("I2C Scanner");

  //By default the Qwiic power is on. But it can be controlled.
  pinMode(pin_qwiicPower, OUTPUT);
  digitalWrite(pin_qwiicPower, HIGH);

  qwiicPowerOff();
  delay(2000); //Power LED on a Qwiic board should now be off

  qwiicPowerOn();

  Wire.begin();
}

void loop()
{
  //Toggle the power on the Qwiic connector

  Serial.println();
  for (byte address = 1; address < 127; address++ )
  {
    Wire.beginTransmission(address);
    if (Wire.endTransmission() == 0)
    {
      Serial.print("Device found at address 0x");
      if (address < 0x10)
        Serial.print("0");
      Serial.println(address, HEX);
    }
  }
  Serial.println("Done");
  delay(100);
}

void qwiicPowerOff()
{
  digitalWrite(pin_qwiicPower, LOW);
}

void qwiicPowerOn()
{
  digitalWrite(pin_qwiicPower, HIGH);
}
