/*
  Reading the fuel gauge on the SparkFun ESP32-S3 Thing Plus
  By: Nathan Seidle
  SparkFun Electronics
  Date: January 13, 2024
  License: MIT. Please see LICENSE.md for more information.

  To use: Attach a LiPo to the JST connector. The I2C pullups are tied to Qwiic power 
  so if Qwiic power is off, the pullups are off.

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

#include <Wire.h> // Needed for I2C

#include <SparkFun_MAX1704x_Fuel_Gauge_Arduino_Library.h> // Click here to get the library: http://librarymanager/All#SparkFun_MAX1704x_Fuel_Gauge_Arduino_Library
SFE_MAX1704X lipo(MAX1704X_MAX17048); // Allow access to all the 17048 features

int pin_qwiicPower = 45; //Thing Plus S3 45 is connected to the v-reg that controls the Qwiic power

void setup()
{
  Serial.begin(115200); // Start serial, to output debug data
  while (Serial == false); //Wait for serial monitor to connect before printing anything
  Serial.println("MAX17048 Example");

  //Power on Qwiic bus to enable I2C pullups
  pinMode(pin_qwiicPower, OUTPUT);
  digitalWrite(pin_qwiicPower, HIGH);

  Wire.begin();
  
   // Set up the MAX17048 LiPo fuel gauge:
  if (lipo.begin() == false) // Connect to the MAX17048 using the default wire port
  {
    Serial.println("MAX17048 not detected. Please check wiring. Freezing.");
    while (true);
  }
}

void loop()
{
  // Print the variables:
  Serial.print("Voltage: ");
  Serial.print(lipo.getVoltage());  // Print the battery voltage
  Serial.print("V");

  Serial.print(" Percentage: ");
  Serial.print(lipo.getSOC(), 2); // Print the battery state of charge with 2 decimal places
  Serial.print("%");

  Serial.print(" Change Rate: ");
  Serial.print(lipo.getChangeRate(), 2); // Print the battery change rate with 2 decimal places
  Serial.print("%/hr");

  Serial.println();

  delay(500);
}
