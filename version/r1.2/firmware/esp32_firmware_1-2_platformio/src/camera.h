/**
 * camera.h - OV2640 camera setup and capture for the MiniSpeedCam.
 *
 * The PWDN and RESET lines are wired to dedicated MCU GPIOs
 * (CAMERA_PWDN_PIN / CAMERA_RST_PIN, defined in pins.h) so the
 * esp_camera config sets PWDN_GPIO_NUM/RESET_GPIO_NUM to -1 and we
 * toggle them manually in cameraPowerOn().
 *
 * Ownership model:
 *   takePhoto() captures a framebuffer from the OV2640 and stashes the
 *   pointer in an atomic slot internal to camera.cpp. The framebuffer
 *   stays resident in PSRAM (so it does not bloat the regular heap)
 *   until cameraReleasePendingPhoto() returns it to the driver. The
 *   uploader (Core 0) consumes the JPEG through the cameraPendingPhoto*
 *   accessors and is responsible for releasing it once the POST is
 *   done. Callers must NOT manipulate camera_fb_t directly -- the type
 *   is intentionally hidden behind these accessors so that no other
 *   module needs to include esp_camera.h.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

// --- OV2640 to ESP32-S3 pin mapping (board revision 1.2) ---
#define PWDN_GPIO_NUM  -1
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM  16
#define SIOD_GPIO_NUM  4
#define SIOC_GPIO_NUM  5

#define Y2_GPIO_NUM    9
#define Y3_GPIO_NUM    11
#define Y4_GPIO_NUM    12
#define Y5_GPIO_NUM    10
#define Y6_GPIO_NUM    13
#define Y7_GPIO_NUM    18
#define Y8_GPIO_NUM    17
#define Y9_GPIO_NUM    15

#define VSYNC_GPIO_NUM 6
#define HREF_GPIO_NUM  7
#define PCLK_GPIO_NUM  8

/**
 * Power-cycle the OV2640.
 *
 * Drives PWDN low (sensor enabled) and pulses RST low->high to bring
 * the sensor out of reset cleanly. Must be called before cameraSetup().
 */
void cameraPowerOn();

/**
 * Initialize the OV2640 via esp_camera.
 *
 * Configures pin mapping, 20 MHz XCLK, UXGA (1600x1200) JPEG output
 * stored in PSRAM, and applies sensor tweaks (vflip, brightness).
 *
 * @return 1 on success, 0 if esp_camera_init() failed (see Serial log).
 */
int cameraSetup();

/**
 * Capture a JPEG into the pending-photo slot.
 *
 * Also populates `upload.photo_filename`. Does not touch the heap with
 * a base64 copy -- the JPEG stays in PSRAM until released.
 *
 * @return true if a fresh frame was captured and is now pending upload.
 *         false if the previous photo has not yet been released by the
 *         uploader (caller should skip this event) or if the OV2640
 *         capture itself failed.
 */
bool takePhoto();

/** @return true while a captured JPEG is waiting in the pending slot. */
bool cameraHasPendingPhoto();

/** @return pointer to the pending JPEG bytes, or nullptr if none. */
const uint8_t* cameraPendingPhotoData();

/** @return size of the pending JPEG in bytes, or 0 if none. */
size_t cameraPendingPhotoLength();

/**
 * Return the pending framebuffer to the camera driver. Idempotent --
 * safe to call when no photo is pending.
 */
void cameraReleasePendingPhoto();

/**
 * Power the OV2640 down for sleep. Releases the pending framebuffer,
 * de-inits the camera driver, and drives PWDN high so the sensor
 * itself stops drawing current. Pair with cameraPowerOn() +
 * cameraSetup() after wake to bring it back.
 */
void cameraPowerOff();

// --- Runtime-tunable image settings (persisted in NVS, edited via ESPUI) ---
//
// Frame size is stored as the framesize_t enum value (an int 0..N). The
// camera module owns the esp_camera coupling so other modules can pass
// raw ints without including esp_camera.h.

int  cameraGetJpegQuality();   // 0..63; lower = better quality
void cameraSetJpegQuality(int quality);

int  cameraGetFrameSize();     // framesize_t cast to int
void cameraSetFrameSize(int framesize);
