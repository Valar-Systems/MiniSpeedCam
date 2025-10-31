# MiniSpeedCam

## Intro
The MiniSpeedCam tracks vehicle speeds, takes a photo, and uploads the photo and speed data to a server.

It can be purchased at [valarsystems.com](https://valarsystems.com/products/minispeedcam).

Use it to track vehicle speeds on your driveway or street.

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

Here's a short video showing it in action. Notice the Max Speed reading in the serial monitor:

Max distance is about 100ft or 30m


## Reflashing the Device

In case you want to reflash your device with other firmware, you may use the stm32loader python module directly :

pip install stm32loader

... then with the USB connected to the device (as it uses RTS/CTS signals to control the MCU nRESET and BOOT0 pins):

```
stm32loader -b 115200 -p com_port -e -w -v -s -f F3 MiniSpeedCam.bin
```

If uploading fails, you may use the STM32 Flash loader after performing the following toggling sequence:

Press the R0 button
Press the B0 button
Release R0
Releaae B0

