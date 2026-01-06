# MiniSpeedCam R1.0

#### STM32 flashing

R1.0 has a problem with the automatic download circuit on the STM32.

The STM32 **MUST** be entered into Boot mode manually using the B0 and R0 button.

First plug your USB cable into the STM32 USB port. Then use the stm32loader python module directly :

```
pip install stm32loader
```

... then with the USB connected to the device.

In the terminal, navigate to the folder that houses the binary, in the Release folder and run the following command. Replace the "com_port" with the correct COM port.

```
stm32loader -b 115200 -p com_port -e -w -v -s -f F3 MiniSpeedCam.bin
```

1. Press the R0 button
2. Press the B0 button
3. Release R0
4. Release B0

#### ESP32 flashing

Open the Arduino sketch in ArduinoIDE. Be sure you have the libraries at the top of the sketch installed. And be sure your upload options are set up according to the image file in the ESP32_firmware folder.

Optional: ArduinoOTA is also included, allowing you to remotely flash the device from the ArduinoIDE. One caveat is the ESP32 goes to sleep after 10 minutes of being powered on. Just cycle the power to it and you'll have 10 minutes to upload your sketch.

