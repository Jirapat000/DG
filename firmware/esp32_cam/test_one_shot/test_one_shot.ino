#include "esp_camera.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include "esp_system.h"
#include "secrets.h"

// Minimal prototype: camera -> Wi-Fi -> HTTPS capture API -> model analysis.
// No sensors, flash, SD, telemetry, sleep, periodic uploads, or web commands.
static const uint32_t WIFI_TIMEOUT_MS = 30000;
static const uint32_t API_TIMEOUT_MS = 60000;

// AI Thinker ESP32-CAM pin map.
static const int PWDN_GPIO_NUM = 32, RESET_GPIO_NUM = -1;
static const int XCLK_GPIO_NUM = 0, SIOD_GPIO_NUM = 26, SIOC_GPIO_NUM = 27;
static const int Y9_GPIO_NUM = 35, Y8_GPIO_NUM = 34, Y7_GPIO_NUM = 39, Y6_GPIO_NUM = 36;
static const int Y5_GPIO_NUM = 21, Y4_GPIO_NUM = 19, Y3_GPIO_NUM = 18, Y2_GPIO_NUM = 5;
static const int VSYNC_GPIO_NUM = 25, HREF_GPIO_NUM = 23, PCLK_GPIO_NUM = 22;

static bool cameraReady = false;

bool setupCamera() {
  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM; config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM; config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM; config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM; config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM; config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM; config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM; config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM; config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_VGA;
  config.jpeg_quality = 14;
  config.fb_count = 1;
  config.grab_mode = CAMERA_GRAB_LATEST;
  config.fb_location = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
  const esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) Serial.printf("CAMERA INIT FAILED: 0x%x\n", err);
  return err == ESP_OK;
}

bool connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return true;
  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("Connecting to Wi-Fi '%s'", WIFI_SSID);
  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT_MS) {
    delay(300);
    Serial.print('.');
  }
  Serial.println();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("WIFI FAILED: status=%d\n", (int)WiFi.status());
    return false;
  }
  Serial.printf("WIFI CONNECTED: IP=%s RSSI=%d dBm\n",
                WiFi.localIP().toString().c_str(), WiFi.RSSI());
  return true;
}

bool syncClock() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  struct tm info;
  if (!getLocalTime(&info, 15000)) {
    Serial.println("NTP FAILED: no valid timestamp");
    return false;
  }
  return true;
}

bool captureAndAnalyze() {
  if (!cameraReady) {
    Serial.println("CAPTURE FAILED: camera is not ready");
    return false;
  }
  if (!connectWiFi() || !syncClock()) return false;

  camera_fb_t *frame = esp_camera_fb_get();
  if (!frame || frame->format != PIXFORMAT_JPEG || frame->len < 1000) {
    if (frame) esp_camera_fb_return(frame);
    Serial.println("CAPTURE FAILED: invalid JPEG frame");
    return false;
  }
  Serial.printf("CAPTURE OK: JPEG %u bytes, %ux%u\n",
                (unsigned)frame->len, frame->width, frame->height);

  WiFiClientSecure client;
  client.setCACert(ROOT_CA);
  client.setTimeout(API_TIMEOUT_MS / 1000);
  client.setHandshakeTimeout(20);
  Serial.printf("HTTPS CONNECT: %s:%u\n", API_HOST, API_PORT);
  if (!client.connect(API_HOST, API_PORT)) {
    esp_camera_fb_return(frame);
    Serial.println("UPLOAD FAILED: TLS/DNS connection failed");
    return false;
  }

  char timestamp[25];
  struct tm info;
  if (!getLocalTime(&info, 1000) ||
      !strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &info)) {
    esp_camera_fb_return(frame);
    client.stop();
    Serial.println("UPLOAD FAILED: timestamp unavailable");
    return false;
  }

  const String boundary = "----DGPrototypeCaptureBoundary7c24";
  String part;
  part.reserve(420);
  part += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"device_id\"\r\n\r\n";
  part += String(DEVICE_ID) + "\r\n--" + boundary + "\r\nContent-Disposition: form-data; name=\"captured_at\"\r\n\r\n";
  part += String(timestamp) + "\r\n--" + boundary + "\r\nContent-Disposition: form-data; name=\"device_token\"\r\n\r\n";
  part += String(DEVICE_TOKEN) + "\r\n";
  part += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"image\"; filename=\"prototype.jpg\"\r\nContent-Type: image/jpeg\r\n\r\n";
  const String ending = "\r\n--" + boundary + "--\r\n";
  const size_t total = part.length() + frame->len + ending.length();

  client.print(String("POST ") + API_PATH + " HTTP/1.1\r\n" +
               "Host: " + API_HOST + "\r\n" +
               "Content-Type: multipart/form-data; boundary=" + boundary + "\r\n" +
               "Content-Length: " + String(total) + "\r\nConnection: close\r\n\r\n");
  size_t sent = client.write((const uint8_t *)part.c_str(), part.length());
  sent += client.write(frame->buf, frame->len);
  sent += client.write((const uint8_t *)ending.c_str(), ending.length());
  esp_camera_fb_return(frame);
  Serial.printf("UPLOAD: %u / %u bytes\n", (unsigned)sent, (unsigned)total);
  if (sent != total) {
    client.stop();
    Serial.println("UPLOAD FAILED: incomplete request body");
    return false;
  }

  String status = client.readStringUntil('\n');
  status.trim();
  Serial.printf("API STATUS: %s\n", status.length() ? status.c_str() : "no response");
  uint32_t started = millis();
  while (client.connected() && millis() - started < API_TIMEOUT_MS) {
    String header = client.readStringUntil('\n');
    header.trim();
    if (!header.length()) break;
    Serial.printf("HEADER: %s\n", header.c_str());
  }
  Serial.println("API BODY BEGIN");
  size_t bodySize = 0;
  while ((client.connected() || client.available()) && millis() - started < API_TIMEOUT_MS && bodySize < 2048) {
    while (client.available() && bodySize < 2048) {
      Serial.write((uint8_t)client.read());
      ++bodySize;
    }
    delay(5);
  }
  Serial.println("\nAPI BODY END");
  client.stop();
  const bool ok = status.indexOf(" 200 ") >= 0 || status.indexOf(" 201 ") >= 0;
  Serial.println(ok ? "RESULT: image analyzed by backend" : "RESULT: upload/analyze failed");
  return ok;
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== ESP32-CAM image analysis prototype ===");
  Serial.printf("RESET REASON: %d\n", (int)esp_reset_reason());
  Serial.println("Sensors=off Flash=off SD=off; VGA single-frame capture");
  cameraReady = setupCamera();
  Serial.printf("CAMERA: %s PSRAM=%s\n", cameraReady ? "ready" : "failed", psramFound() ? "yes" : "no");
  if (cameraReady && connectWiFi()) {
    captureAndAnalyze(); // One automatic capture after each boot.
  }
  Serial.println("Ready. Send 'c' in Serial Monitor to capture and analyze again.");
}

void loop() {
  if (Serial.available()) {
    const char command = (char)Serial.read();
    while (Serial.available()) Serial.read();
    if (command == 'c' || command == 'C') captureAndAnalyze();
  }
  delay(20);
}
