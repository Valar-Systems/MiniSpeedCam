#include "camera.h"

#include <atomic>

#include <Arduino.h>
#include <esp_camera.h>

#include "pins.h"
#include "variables.h"

// Single-slot framebuffer ownership for the Core 1 -> Core 0 hand-off.
// Core 1 stores a non-null pointer in takePhoto(); Core 0 swaps it back
// to null in cameraReleasePendingPhoto() and returns it to the driver.
// std::atomic gives both cores well-defined visibility around the
// exchange; the JPEG payload itself lives in PSRAM and is read-only
// while a non-null fb is published.
static std::atomic<camera_fb_t*> g_pending_fb{nullptr};

// Persisted image settings. Defaults match the original sketch (UXGA at
// quality 20); both are overwritten in cameraSetup() if the user has
// previously set different values via ESPUI.
static int          g_jpeg_quality = 20;
static framesize_t  g_frame_size   = FRAMESIZE_UXGA;

void cameraPowerOn() {
  digitalWrite(CAMERA_PWDN_PIN, LOW);
  digitalWrite(CAMERA_RST_PIN, LOW);
  delay(10);
  digitalWrite(CAMERA_RST_PIN, HIGH);
  delay(10);
}

int cameraSetup() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;  // 20 mzh or 16 mhz

  /*
  FRAMESIZE_UXGA (1600 x 1200)
  FRAMESIZE_QVGA (320 x 240)
  FRAMESIZE_CIF (352 x 288)
  FRAMESIZE_VGA (640 x 480)
  FRAMESIZE_SVGA (800 x 600)
  FRAMESIZE_XGA (1024 x 768)
  FRAMESIZE_SXGA (1280 x 1024)
  */

  // Pull persisted settings out of NVS so the camera comes up with the
  // user's last-chosen frame size / quality. Requires preferences to be
  // open before cameraSetup() is called (main.cpp guarantees this).
  g_jpeg_quality = constrain(preferences.getInt("jpeg_q", 20), 0, 63);
  g_frame_size   = (framesize_t)preferences.getInt("frame_size", (int)FRAMESIZE_UXGA);

  config.frame_size = g_frame_size;
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode = CAMERA_GRAB_LATEST;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = g_jpeg_quality;  // 0..63, lower = better quality
  config.fb_count = 1;

  // camera init
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x", err);
    return 0;
  }

  sensor_t* s = esp_camera_sensor_get();
  // initial sensors are flipped vertically and colors are a bit saturated
  s->set_vflip(s, 1);       // 0 = disable , 1 = enable
  s->set_hmirror(s, 0);     // 0 = disable , 1 = enable
  s->set_brightness(s, 1);  // up the brightness just a bit
  s->set_saturation(s, 0);  // lower the saturation

  // First few frames after init are typically dark / banded while the
  // OV2640 settles its AGC and exposure. Pull and discard a handful so
  // the first user-visible capture is a good one.
  for (int i = 0; i < 3; ++i) {
    camera_fb_t* warmup_fb = esp_camera_fb_get();
    if (warmup_fb) esp_camera_fb_return(warmup_fb);
  }

  Serial.println("Camera configuration complete!");
  return 1;
}

void cameraPowerOff() {
  // Tear down the driver first so its DMA descriptors and PSRAM buffer
  // are released, then drive PWDN high to actually cut sensor power.
  // The pending-photo slot is cleared as a safety measure -- it should
  // already be null by the time we get here.
  cameraReleasePendingPhoto();
  esp_camera_deinit();
  digitalWrite(CAMERA_PWDN_PIN, HIGH);
}

int cameraGetJpegQuality() { return g_jpeg_quality; }

void cameraSetJpegQuality(int quality) {
  quality = constrain(quality, 0, 63);
  g_jpeg_quality = quality;
  preferences.putInt("jpeg_q", quality);
  if (sensor_t* s = esp_camera_sensor_get()) {
    s->set_quality(s, quality);
  }
}

int cameraGetFrameSize() { return (int)g_frame_size; }

void cameraSetFrameSize(int framesize) {
  framesize_t fs = (framesize_t)framesize;
  g_frame_size = fs;
  preferences.putInt("frame_size", framesize);
  if (sensor_t* s = esp_camera_sensor_get()) {
    s->set_framesize(s, fs);
  }
}

bool takePhoto() {
  // Don't trample a framebuffer that the uploader hasn't released yet.
  // With fb_count = 1 there is exactly one buffer in flight; releasing
  // it twice (or capturing on top of it) would underflow the driver's
  // internal accounting.
  if (g_pending_fb.load() != nullptr) {
    Serial.println("takePhoto: previous photo still pending; skipping capture");
    return false;
  }

  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Camera capture failed 1");
    return false;
  }

  // Populate the filename *before* publishing the framebuffer pointer so
  // that the release-store on g_pending_fb acts as a memory fence for
  // upload.photo_filename as well.
  upload.photo_filename = "image.jpg";
  g_pending_fb.store(fb);
  return true;
}

bool cameraHasPendingPhoto() {
  return g_pending_fb.load() != nullptr;
}

const uint8_t* cameraPendingPhotoData() {
  camera_fb_t* fb = g_pending_fb.load();
  return fb ? fb->buf : nullptr;
}

size_t cameraPendingPhotoLength() {
  camera_fb_t* fb = g_pending_fb.load();
  return fb ? fb->len : 0;
}

void cameraReleasePendingPhoto() {
  camera_fb_t* fb = g_pending_fb.exchange(nullptr);
  if (fb) {
    esp_camera_fb_return(fb);
  }
}
