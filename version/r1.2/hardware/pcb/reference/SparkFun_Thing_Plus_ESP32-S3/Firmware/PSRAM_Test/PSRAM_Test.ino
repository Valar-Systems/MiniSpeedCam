/*
  Enable PSRAM on the ESP32-S3
  By: Nathan Seidle
  SparkFun Electronics
  Date: January 13, 2024
  License: MIT. Please see LICENSE.md for more information.

  This example shows how to enable PSRAM on ESP32-S3 modules that have it, and use it for
  RAM requests above a certain byte threshold.

  Note: Not all ESP32 modules have PSRAM built-in. The SparkFun ESP32-S3 uses the 
  ESP32-S3 Mini N8R2 with 8MB flash and 2MB PSRAM. 

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

unsigned long lastHeapReport;

void setup()
{
  Serial.begin(115200);
  while (Serial == false); //Wait for serial monitor to connect before printing anything

  if (psramInit() == false)
    Serial.println("PSRAM failed to initialize");
  else
    Serial.println("PSRAM initialized");

  Serial.printf("PSRAM Size available (bytes): %d\r\n", ESP.getFreePsram());

  heap_caps_malloc_extmem_enable(1000); //Use PSRAM for memory requests larger than 1,000 bytes
}

void loop()
{
  delay(20);

  if (millis() - lastHeapReport > 1000)
  {
    lastHeapReport = millis();
    reportHeap();
  }

  if (Serial.available()) ESP.restart();
}

void reportHeap()
{
  Serial.printf("FreeHeap: %d / HeapLowestPoint: %d / LargestBlock: %d / Used PSRAM: %d\r\n", ESP.getFreeHeap(),
                xPortGetMinimumEverFreeHeapSize(), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), ESP.getPsramSize() - ESP.getFreePsram());
}
