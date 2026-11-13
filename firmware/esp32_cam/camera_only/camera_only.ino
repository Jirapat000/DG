#include "esp_camera.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include "secrets.h"

// Camera-only firmware: JPEG snapshot -> Render API -> detection/LINE on backend.
// No flow sensor or float switch code/fields are included in this sketch.
static const uint32_t CAPTURE_INTERVAL_MS = 5UL * 60UL * 1000UL;
static const uint32_t WIFI_TIMEOUT_MS = 30000;
static const uint32_t API_RETRY_DELAY_MS = 5000;
static const uint8_t API_RETRIES = 3;

// AI Thinker ESP32-CAM pin map.
static const int PWDN_GPIO_NUM = 32;
static const int RESET_GPIO_NUM = -1;
static const int FLASH_LED_GPIO_NUM = 4;
static const int XCLK_GPIO_NUM = 0;
static const int SIOD_GPIO_NUM = 26;
static const int SIOC_GPIO_NUM = 27;
static const int Y9_GPIO_NUM = 35;
static const int Y8_GPIO_NUM = 34;
static const int Y7_GPIO_NUM = 39;
static const int Y6_GPIO_NUM = 36;
static const int Y5_GPIO_NUM = 21;
static const int Y4_GPIO_NUM = 19;
static const int Y3_GPIO_NUM = 18;
static const int Y2_GPIO_NUM = 5;
static const int VSYNC_GPIO_NUM = 25;
static const int HREF_GPIO_NUM = 23;
static const int PCLK_GPIO_NUM = 22;

static bool cameraReady = false;

bool setupCamera() {
  camera_config_t config = {};
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
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = psramFound() ? FRAMESIZE_SVGA : FRAMESIZE_VGA;
  config.jpeg_quality = 12;
  config.fb_count = psramFound() ? 2 : 1;
  config.grab_mode = CAMERA_GRAB_LATEST;
  config.fb_location = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed: 0x%x\n", err);
    return false;
  }
  return true;
}

bool connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("Connecting to Wi-Fi: %s", WIFI_SSID);
  uint32_t started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < WIFI_TIMEOUT_MS) {
    delay(300);
    Serial.print('.');
  }
  Serial.println();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Wi-Fi connection timed out");
    return false;
  }
  Serial.print("Wi-Fi connected; camera IP: ");
  Serial.println(WiFi.localIP());
  return true;
}

bool getUtcTimestamp(char *buffer, size_t bufferSize) {
  struct tm timeInfo;
  if (!getLocalTime(&timeInfo, 15000)) return false;
  return strftime(buffer, bufferSize, "%Y-%m-%dT%H:%M:%SZ", &timeInfo) > 0;
}

bool uploadFrame(camera_fb_t *frame) {
  if (!frame || frame->format != PIXFORMAT_JPEG) return false;

  char timestamp[25];
  if (!getUtcTimestamp(timestamp, sizeof(timestamp))) {
    Serial.println("Clock is not synchronized; skipping upload");
    return false;
  }

  WiFiClientSecure client;
  client.setCACert(ROOT_CA);
  client.setTimeout(20000);
  if (!client.connect(API_HOST, API_PORT)) {
    Serial.println("HTTPS connection to API failed");
    return false;
  }

  const String boundary = "----DrainWatchCameraOnlyBoundary7MA4YWxkTrZu0gW";
  String bodyStart;
  bodyStart.reserve(512);
  bodyStart += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"device_id\"\r\n\r\n";
  bodyStart += String(DEVICE_ID) + "\r\n";
  bodyStart += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"captured_at\"\r\n\r\n";
  bodyStart += String(timestamp) + "\r\n";
  bodyStart += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"device_token\"\r\n\r\n";
  bodyStart += String(DEVICE_TOKEN) + "\r\n";
  bodyStart += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"image\"; filename=\"capture.jpg\"\r\nContent-Type: image/jpeg\r\n\r\n";

  const String bodyEnd = "\r\n--" + boundary + "--\r\n";
  const size_t contentLength = bodyStart.length() + frame->len + bodyEnd.length();
  client.print(String("POST ") + API_PATH + " HTTP/1.1\r\n" +
               "Host: " + API_HOST + "\r\n" +
               "Content-Type: multipart/form-data; boundary=" + boundary + "\r\n" +
               "Content-Length: " + String(contentLength) + "\r\n" +
               "Connection: close\r\n\r\n");

  size_t sent = client.write((const uint8_t *)bodyStart.c_str(), bodyStart.length());
  sent += client.write(frame->buf, frame->len);
  sent += client.write((const uint8_t *)bodyEnd.c_str(), bodyEnd.length());
  if (sent != contentLength) {
    Serial.println("Image upload was incomplete");
    client.stop();
    return false;
  }

  String statusLine = client.readStringUntil('\n');
  statusLine.trim();
  Serial.print("API response: ");
  Serial.println(statusLine);
  client.stop();
  return statusLine.indexOf(" 200 ") >= 0;
}

bool captureAndUpload() {
  if (!cameraReady) return false;
  camera_fb_t *frame = esp_camera_fb_get();
  if (!frame) {
    Serial.println("Camera capture failed");
    return false;
  }

  Serial.printf("Captured JPEG: %u bytes\n", (unsigned)frame->len);
  bool uploaded = false;
  for (uint8_t attempt = 1; attempt <= API_RETRIES && !uploaded; ++attempt) {
    if (WiFi.status() != WL_CONNECTED && !connectWiFi()) break;
    uploaded = uploadFrame(frame);
    if (!uploaded && attempt < API_RETRIES) {
      Serial.printf("Retrying upload (%u/%u)\n", attempt + 1, API_RETRIES);
      delay(API_RETRY_DELAY_MS);
    }
  }

  esp_camera_fb_return(frame);
  Serial.println(uploaded ? "Capture analyzed by backend" : "Capture upload failed");
  return uploaded;
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("Drain Watch camera-only firmware");
  Serial.println("Sensors: disabled/not included");
  pinMode(FLASH_LED_GPIO_NUM, OUTPUT);
  digitalWrite(FLASH_LED_GPIO_NUM, HIGH);
  Serial.println("Camera flash LED: on while the board is powered");

  cameraReady = setupCamera();
  if (!cameraReady) return;

  if (!connectWiFi()) return;
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  captureAndUpload();
}

void loop() {
  delay(CAPTURE_INTERVAL_MS);
  if (cameraReady) captureAndUpload();
}
