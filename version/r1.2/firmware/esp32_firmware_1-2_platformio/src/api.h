/**
 * api.h - WiFi management and HTTPS uploads to minispeedcam.com.
 *
 * Provides:
 *   - connectWifiAP():   first-boot bring-up; tries STA, otherwise opens
 *                        a "MiniSpeedCam" soft-AP for the captive ESPUI.
 *   - connectWifi():     reconnect helper used by Core 0 when the radar
 *                        wakes the device and STA is dropped.
 *   - wifiResetButton(): 3-second long-press on WIFI_RESET_PIN clears
 *                        stored credentials and reboots into AP mode.
 *   - sendLocalIP():     announce the device's LAN IP to the cloud so
 *                        the user can reach the ESPUI portal remotely.
 *   - sendPhoto():       POST the captured photo + max speed (or just
 *                        the speed when below the photo threshold).
 *
 * NOTE: the Bearer token used inside sendLocalIP()/sendPhoto() is a
 * public Bubble.io workflow key for the minispeedcam.com endpoints, not
 * a per-device secret.
 */
#pragma once

#define HOSTNAME "MiniSpeedCam"  // mDNS name and Soft-AP SSID

void connectWifiAP();
void connectWifi();
void wifiResetButton();
void sendLocalIP();
void sendPhoto();
