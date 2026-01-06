# MiniSpeedCam R1.1

Hardware version R1.1 has a problem with the ESP32 USB circuit.

When the ESP32 enters sleep mode, the USB becomes disconnected and the Serial Console will not function after it wakes up. Arduino may need to be restarted or the USB cable may need to be disconnected and reconnected.

For this reason, do not enter sleep mode when debugging.
