# MiniSpeedCam

## Intro
The MiniSpeedCam tracks vehicle speeds, takes a photo, and uploads the photo and speed data to a server. Use it to track vehicle speeds on your driveway or street.

**Please sign up to the waitlist at [minispeedcam.com](https://minispeedcam.com/) if you're interested in purchasing one. We have not began manufacturing these yet.**

<div style="text-align: center;">
<img src="/docs/images/front-components.jpg" width="600">
</div>
<img src="/docs/images/back-components.jpg" width="600">

The firmware is designed to work with [minispeedcam.com](https://minispeedcam.com/) and requires **absolutely no coding or flashing.** The device works right out of the box.

The device creates a WiFi access point when started for the first time. Connect to it with your phone or laptop and enter your WiFi credentials. It will automatically connect to the internet.

All photos and speed data is sent to your account on [minispeedcam.com](https://minispeedcam.com/) where the data can be viewed and downloaded.

If you prefer to use your own server, just modify the Arduino code to point to wherever you'd like. It's completely open-source.


## Special thanks

This device wouldn't be possible without the help of [Mathieu (aka Limpkin)](https://www.limpkin.fr/index.php?post/2022/03/31/CDM324-Doppler-Motion-Sensor-Backpack%2C-now-with-FFTs%21). Thank you for your CDM324 Backpack and your generous help in getting MiniSpeedCam made.


## Accuracy and distance

A 100Hz frequency from the CDM324 corresponds to 2.3km/h speed. By using a tuning fork, we can trick the radar sensor into picking up a specific speed. Using a tuning fork with a frequency of 2048 Hz should trigger a speed reading of 47.1 kph or 29.3 mph ((2048/100) x 2.3)

[Here's a short video](https://youtu.be/ftSh7Fsy7hw) of the tuning fork in action.

Max distance is about 100ft or 30m

## How it works

The device uses the CDM324 which output a very small voltage, depending on how fast the vehicle is traveling. This voltage is amplified by the aplification circuit and sent to the STM32 for processing. The STM32 uses FFT to determine the speed of the vehicle.

The ESP32 handles the speed data, camera, and WiFi. It is connected to the STM32 via UART and polls it for speed data. Once the speed crosses a threshold that you set, the ESP32 takes a photo and sends a POST API call to a server with the data and image encoded in base64. The vehicles maximum speed, image, and date/time are then saved to a server.


## Battery operation

The device consumes about 1.10W while in full operation, and about 0.55W while in sleep mode. The radar sensor is always on, thus consuming 0.55W at all time. When movement is detected, the ESP32 wakes up from light sleep mode, turns on the camera, connects to wifi, takes a photo, sends the data, and goes back to sleep. The process takes about 10 seconds from start to finish. Battery life will depend on how many vehicles the sensor picks up, thus determing how often the ESP32 wakes up. The more vehicles on the road, the larger the battery drain will be.

## Power source

MiniSpeedCam runs on 5V. There are two USB-C ports and a screw terminal that can be used to supply 5V power. The easiest way to power this device is to use an external battery pack with a USB-C cable and plug it into either USB-C plug.

## Reflashing the device

Flashing the firmware is very simple. The STM32 is programmed in STM32 Cube IDE and the ESP32 is programmed in Arduino IDE.  

#### STM32 flashing

First plug your USB cable into the STM32 USB port. Then use the stm32loader python module directly :

```
pip install stm32loader
```

... then with the USB connected to the device (as it uses RTS/CTS signals to control the MCU nRESET and BOOT0 pins):

```
stm32loader -b 115200 -p com_port -e -w -v -s -f F3 MiniSpeedCam.bin
```

If uploading fails, you may use the STM32 Flash loader after performing the following toggling sequence:

1. Press the R0 button
2. Press the B0 button
3. Release R0
4. Release B0

#### ESP32 flashing

Open the Arduino sketch in ArduinoIDE. Be sure you have the libraries at the top of the sketch installed. And be sure your upload options are set up according to the image file in the ESP32_firmware folder.

Optional: ArduinoOTA is also included, allowing you to remotely flash the device from the ArduinoIDE. One caveat is the ESP32 goes to sleep after 10 minutes of being powered on. Just cycle the power to it and you'll have 10 minutes to upload your sketch.

