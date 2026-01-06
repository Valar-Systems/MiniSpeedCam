# MiniSpeedCam R1.1

Hardware version R1.1 has a problem with the ESP32 USB circuit.

When the ESP32 enters sleep mode, the USB becomes disconnected and the Serial Console will not function after it wakes up. Arduino may need to be restarted or the USB cable may need to be disconnected and reconnected.

More info here: https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-guides/usb-serial-jtag-console.html#sleep-mode-considerations

For this reason, do not enter sleep mode when debugging.

ESP32 Upload Settings. **Menu -> Tools**

- USB CDC On Boot: Enabled
- CPU Frequency: 240MHz (WiFi)
- Core Debug Level: Verbose
- Events Run On: Core 1
- Flash Size: 8MB 
- Arduino Runs On: Core 1
- Partition Scheme: 8MB with spiffs
- PSRAM: OPI PSRAM

