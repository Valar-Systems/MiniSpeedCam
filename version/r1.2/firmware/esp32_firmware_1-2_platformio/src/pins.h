/**
 * pins.h - GPIO assignments for MiniSpeedCam hardware revision 1.2.
 *
 * Shared across the main entry point and every module so that pin
 * numbering only ever lives in one place.
 */
#pragma once

#include <Arduino.h>

// --- ESP32-S3 <-> STM32 radar MCU ---
#define STM32_RESET_PIN GPIO_NUM_47  // Drives STM32 NRST low to reset the radar MCU
#define RX_GPIO         42           // UART1 RX from STM32 (speed reports)
#define TX_GPIO         41           // UART1 TX to STM32 (speed query commands)
#define ESP_WAKEUP_PIN  GPIO_NUM_1   // STM32 pulls HIGH when motion >= ~5mph is detected
#define STM_WAKEUP_PIN  GPIO_NUM_2   // Reserved: ESP -> STM wake signal

// --- User input ---
#define WIFI_RESET_PIN  GPIO_NUM_21  // Active-low button: hold 3s to clear stored WiFi creds

// --- OV2640 camera control ---
#define CAMERA_PWDN_PIN GPIO_NUM_45  // OV2640 power-down (active high)
#define CAMERA_RST_PIN  GPIO_NUM_19  // OV2640 reset (active low)
