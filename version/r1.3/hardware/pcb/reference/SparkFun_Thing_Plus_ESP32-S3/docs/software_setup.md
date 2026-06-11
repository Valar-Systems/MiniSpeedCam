---
icon: simple/arduino
---

!!! attention
	If this is your first time using Arduino, please read through our tutorial on [installing the Arduino IDE](https://learn.sparkfun.com/tutorials/installing-arduino-ide). If you have not installed an Arduino library before, we recommend you check out our [installation guide](https://learn.sparkfun.com/tutorials/installing-an-arduino-library).

With the ESP32-S3 Thing Plus connected to our computer, it's time to set up the boards package in Arduino.

## Installing espressif Arduino Boards
    
To install the ESP32 boards package, open the Preferences menu by navigating to <b>File</b> > <b>Preferences</b>. Look at the bottom of the Preferences menu for "Additional boards manager URLS" and then copy this JSON link into that field:

<code>
    https://espressif.github.io/arduino-esp32/package_esp32_dev_index.json
</code>

Click "Ok" and then open the *Boards Manager* tool, search for "espressif ESP32" and install the latest alpha release (3.0.0-alpha2 or later). This install process may take some time so feel free to step away while it downloads and installs.

## Optional Arduino Libraries

Most of the examples we cover in the next section use existing libraries included with the ESP32 Arduino core but the RGB LED example uses the FastLED library so you'll need it installed to follow along with that example. Navigate to the "Libraries" menu on the left side of the screen, search for "FastLED", and install the latest version. If you prefer to manually install the library, you can download a ZIP of the library by clicking the button below:

<center>
[FastLED Arduino Library](https://github.com/FastLED/FastLED/archive/refs/heads/master.zip){ .md-button .md-button--primary }   
</center>

## espressif IDF

Users who prefer to use espressif's development toolkit, espressif IDF, can get started by following their instructions [here](https://www.espressif.com/en/products/sdks/esp-idf) and  ESP32-S3 specific documentation [here](https://docs.espressif.com/projects/esp-idf/en/stable/esp32S3/index.html). 