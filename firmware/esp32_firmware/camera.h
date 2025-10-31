#define PWDN_GPIO_NUM -1
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM 16
#define SIOD_GPIO_NUM 4
#define SIOC_GPIO_NUM 5

#define Y2_GPIO_NUM 9
#define Y3_GPIO_NUM 11
#define Y4_GPIO_NUM 12
#define Y5_GPIO_NUM 10
#define Y6_GPIO_NUM 13
#define Y7_GPIO_NUM 18
#define Y8_GPIO_NUM 17
#define Y9_GPIO_NUM 15

#define VSYNC_GPIO_NUM 6
#define HREF_GPIO_NUM 7
#define PCLK_GPIO_NUM 8

void cameraPowerOn() {
  digitalWrite(CAMERA_PWDN_PIN, LOW);
  digitalWrite(CAMERA_RST_PIN, LOW);
  delay(10);
  digitalWrite(CAMERA_RST_PIN, HIGH);
  delay(10);
}

int cameraSetup(void) {
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

  config.frame_size = FRAMESIZE_UXGA;
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode = CAMERA_GRAB_LATEST;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = 20; //The image quality (jpeg_quality) can be a number between 0 and 63. Higher is better quality, but causes performance issues
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

  Serial.println("Camera configuration complete!");
  return 1;
}

void takePhoto(void) {
  camera_fb_t* fb = esp_camera_fb_get();  // Capture photo
  if (!fb) {
    Serial.println("Camera capture failed 1");
    return;
  }
  photo_filename = "image.jpg";
  photo_base64 = base64::encode(fb->buf, fb->len);
  esp_camera_fb_return(fb);
}
