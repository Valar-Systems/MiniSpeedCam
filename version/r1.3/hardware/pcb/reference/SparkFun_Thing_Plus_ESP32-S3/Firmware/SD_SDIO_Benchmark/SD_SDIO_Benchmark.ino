/*
  Use the built-in ESP32 SD_MMC library to test the SD write speed over 4-bit SDIO.
  By: Nathan Seidle
  SparkFun Electronics
  Date: January 13, 2024
  License: MIT. Please see LICENSE.md for more information.

  This demonstrates 4-bit SDIO write and read speed averages. We found
  2.5MB/s to ~4.1MB/s on various microSD cards 1GB to 32GB.

  These exmaples are written not to demonstrate the absolute extremes that a library
  may be capable. Instead, we try to emulate normal user experiences, with basic read/writes.

  Shockingly, sdFat performed as fast, or faster than the SDIO 4-bit interface in many circumstances.

  Tested using ESP32 core v2.0.14

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

#include "FS.h"
#include "SD_MMC.h"

//Define pins for SDIO interface
int pin_sdioCLK = 38;
int pin_sdioCMD = 34;
int pin_sdioD0 = 39;
int pin_sdioD1 = 40;
int pin_sdioD2 = 47;
int pin_sdioD3 = 33;

void setup()
{
  Serial.begin(115200);
  while (Serial == false);
  Serial.println();
  Serial.println("SDIO test");

  runTestsSDIO();
}

void loop()
{
  if (Serial.available()) ESP.restart();
}

void runTestsSDIO()
{
  //Define pin interface to SDIO card
  if (SD_MMC.setPins(pin_sdioCLK, pin_sdioCMD, pin_sdioD0, pin_sdioD1, pin_sdioD2, pin_sdioD3) == false)
  {
    Serial.println("SDIO pin assignment failed!");
    return;
  }

  if (SD_MMC.begin() == false)
  {
    Serial.println("Card Mount Failed");
    return;
  }

  uint8_t cardType = SD_MMC.cardType();
  if (cardType == CARD_NONE)
  {
    Serial.println("No SD_MMC card detected");
    return;
  }

  Serial.print("SD_MMC Card Type: ");
  if (cardType == CARD_MMC)
    Serial.println("MMC");
  else if (cardType == CARD_SD)
    Serial.println("SDSC");
  else if (cardType == CARD_SDHC)
    Serial.println("SDHC");
  else
    Serial.println("UNKNOWN");

  Serial.printf("Card Size: %lluMB\n", SD_MMC.cardSize() / (1024 * 1024));
  Serial.printf("Total space: %lluMB\n", SD_MMC.totalBytes() / (1024 * 1024));
  Serial.printf("Used space: %lluMB\n", SD_MMC.usedBytes() / (1024 * 1024));

  float avgWriteSpeed = 0.0;
  float avgReadSpeed = 0.0;
  int avgAmount = 10;

  for (int i = 0 ; i < avgAmount ; i++)
  {
    //10k and 100k report very slow writes
    avgWriteSpeed += fileWriteTest("/test.txt", 500000); //500k is a little slower but mostly indicative of 5MB
    avgReadSpeed += fileReadTest("/test.txt");
  }
  avgWriteSpeed /= avgAmount;
  avgReadSpeed /= avgAmount;

  Serial.printf("avgWriteSpeed: %0.2f avgReadSpeed: %0.2f", avgWriteSpeed, avgReadSpeed);
}

float fileWriteTest(const char *path, uint32_t bytesToWrite)
{
  static uint8_t buf[512];

  //Remove test file if it exists
  SD_MMC.remove(path);

  File file = SD_MMC.open(path, FILE_WRITE);
  if (!file)
  {
    Serial.println("Failed to open file for writing");
    return (0.0);
  }

  randomSeed(micros());

  //Fill buffer with random bytes
  for (int x = 0 ; x < sizeof(buf) ; x++)
    buf[x] = random(0, 256); //Inclusive, exclusive

  unsigned long startTime = millis();
  uint32_t bytesLeftToWrite = bytesToWrite;

  while (bytesLeftToWrite > 0)
  {
    uint16_t bytesWritten = 512;
    if (bytesWritten > bytesLeftToWrite)
      bytesWritten = bytesLeftToWrite;
    bytesWritten = file.write(buf, bytesWritten);
    bytesLeftToWrite -= bytesWritten;
  }

  unsigned long writeTime = millis() - startTime;

  float writeSpeed = bytesToWrite / 1000000. / (writeTime / 1000.);

  Serial.printf("Write Test: %0.2f MB = %0.2f MB/s ", bytesToWrite / 1000000., writeSpeed);

  file.close();

  return (writeSpeed);
}

//Read a given file and print the time taken as well as MB/s
float fileReadTest(const char * path)
{
  File file = SD_MMC.open(path);
  if (!file)
  {
    Serial.println("Failed to open file for reading");
    return (0.0);
  }

  unsigned long bytesLeftToRead = file.size();
  unsigned long fileSize = bytesLeftToRead;
  static uint8_t buf[512];

  unsigned long startTime = millis();
  while (bytesLeftToRead > 0)
  {
    size_t toRead = bytesLeftToRead;
    if (toRead > 512)
      toRead = 512;
    file.read(buf, toRead);
    bytesLeftToRead -= toRead;
  }

  unsigned long readTime = millis() - startTime;

  float readSpeed = fileSize / 1000000. / (readTime / 1000.);
  Serial.printf("Read Test: %0.2f MB = %0.2f MB/s\r\n", fileSize / 1000000., readSpeed);

  file.close();

  return (readSpeed);
}
