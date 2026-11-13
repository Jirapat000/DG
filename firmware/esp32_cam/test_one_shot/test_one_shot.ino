#include "esp_camera.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>
#include "secrets.h"

// One-shot ESP32-CAM diagnostic. Type c in Serial Monitor to capture/upload once.
// No sensors, periodic captures, retries, or SD card are used.
static const uint32_t WIFI_TIMEOUT_MS = 30000;
static const uint32_t API_TIMEOUT_MS = 45000;
static const uint32_t COMMAND_POLL_MS = 5000;

// AI Thinker ESP32-CAM pin map.
static const int PWDN_GPIO_NUM = 32, RESET_GPIO_NUM = -1;
static const int FLASH_LED_GPIO_NUM = 4;
static const int XCLK_GPIO_NUM = 0, SIOD_GPIO_NUM = 26, SIOC_GPIO_NUM = 27;
static const int Y9_GPIO_NUM = 35, Y8_GPIO_NUM = 34, Y7_GPIO_NUM = 39, Y6_GPIO_NUM = 36;
static const int Y5_GPIO_NUM = 21, Y4_GPIO_NUM = 19, Y3_GPIO_NUM = 18, Y2_GPIO_NUM = 5;
static const int VSYNC_GPIO_NUM = 25, HREF_GPIO_NUM = 23, PCLK_GPIO_NUM = 22;
static bool cameraReady = false;
static uint32_t lastCommandPoll = 0;

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
  config.frame_size = psramFound() ? FRAMESIZE_SVGA : FRAMESIZE_VGA;
  config.jpeg_quality = 12;
  config.fb_count = psramFound() ? 2 : 1;
  config.grab_mode = CAMERA_GRAB_LATEST;
  config.fb_location = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) Serial.printf("Camera init failed: 0x%x\n", err);
  return err == ESP_OK;
}

bool connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return true;
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("Connecting to Wi-Fi '%s'", WIFI_SSID);
  const uint32_t started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < WIFI_TIMEOUT_MS) {
    delay(300); Serial.print('.');
  }
  Serial.println();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("Wi-Fi failed; status=%d\n", (int)WiFi.status());
    return false;
  }
  Serial.printf("Wi-Fi connected; IP=%s; RSSI=%d dBm\n",
                WiFi.localIP().toString().c_str(), WiFi.RSSI());
  return true;
}

bool syncClock() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  struct tm timeInfo;
  if (!getLocalTime(&timeInfo, 15000)) {
    Serial.println("Clock sync failed; check internet/DNS");
    return false;
  }
  return true;
}

bool captureAndUploadOnce(const String &commandId = "") {
  if (!cameraReady) { Serial.println("TEST FAIL: camera is not initialized"); return false; }
  if (!connectWiFi()) { Serial.println("TEST FAIL: Wi-Fi unavailable"); return false; }
  if (!syncClock()) { Serial.println("TEST FAIL: no valid timestamp; upload skipped"); return false; }

  camera_fb_t *frame = esp_camera_fb_get();
  if (!frame) { Serial.println("TEST FAIL: esp_camera_fb_get returned null"); return false; }
  Serial.printf("CAPTURE OK: JPEG %u bytes, %ux%u, format=%d\n",
                (unsigned)frame->len, frame->width, frame->height, (int)frame->format);
  if (frame->format != PIXFORMAT_JPEG || frame->len < 1000) {
    Serial.println("TEST FAIL: invalid/too-small JPEG");
    esp_camera_fb_return(frame);
    return false;
  }

  struct tm timeInfo;
  if (!getLocalTime(&timeInfo, 1000)) {
    Serial.println("TEST FAIL: clock became unavailable before upload");
    esp_camera_fb_return(frame);
    return false;
  }
  char timestamp[25];
  strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &timeInfo);
  WiFiClientSecure client;
  client.setCACert(ROOT_CA);
  client.setTimeout(API_TIMEOUT_MS / 1000);
  client.setHandshakeTimeout(20);
  Serial.printf("Connecting HTTPS to %s:%u ...\n", API_HOST, API_PORT);
  if (!client.connect(API_HOST, API_PORT)) {
    Serial.println("TEST FAIL: TLS/DNS/connect failed (no HTTP response)");
    esp_camera_fb_return(frame);
    return false;
  }

  const String boundary = "----DGOneShotBoundary9a72f05c";
  String part;
  part.reserve(420);
  part += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"device_id\"\r\n\r\n";
  part += String(DEVICE_ID) + "\r\n--" + boundary + "\r\nContent-Disposition: form-data; name=\"captured_at\"\r\n\r\n";
  part += String(timestamp) + "\r\n--" + boundary + "\r\nContent-Disposition: form-data; name=\"device_token\"\r\n\r\n";
  part += String(DEVICE_TOKEN) + "\r\n";
  if (commandId.length()) part += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"command_id\"\r\n\r\n" + commandId + "\r\n";
  part += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"image\"; filename=\"esp32cam-test.jpg\"\r\nContent-Type: image/jpeg\r\n\r\n";
  const String ending = "\r\n--" + boundary + "--\r\n";
  const size_t contentLength = part.length() + frame->len + ending.length();

  client.print(String("POST ") + API_PATH + " HTTP/1.1\r\n" +
               "Host: " + API_HOST + "\r\n" +
               "Content-Type: multipart/form-data; boundary=" + boundary + "\r\n" +
               "Content-Length: " + String(contentLength) + "\r\n" +
               "Connection: close\r\n\r\n");
  size_t sent = client.write((const uint8_t *)part.c_str(), part.length());
  sent += client.write(frame->buf, frame->len);
  sent += client.write((const uint8_t *)ending.c_str(), ending.length());
  Serial.printf("UPLOAD: sent %u / %u bytes\n", (unsigned)sent, (unsigned)contentLength);
  esp_camera_fb_return(frame);
  if (sent != contentLength) {
    Serial.println("TEST FAIL: socket accepted only part of the request body");
    client.stop();
    return false;
  }

  String statusLine = client.readStringUntil('\n');
  statusLine.trim();
  Serial.print("API STATUS: "); Serial.println(statusLine.length() ? statusLine : "no response (timeout/disconnect)");
  // Print response headers so 502/413/401 and proxy details can be diagnosed.
  uint32_t bodyStarted = millis();
  while (client.connected() && millis() - bodyStarted < API_TIMEOUT_MS) {
    String line = client.readStringUntil('\n');
    if (!line.length()) break;
    line.trim();
    if (line.length()) { Serial.print("HEADER: "); Serial.println(line); }
  }
  Serial.println("API BODY BEGIN");
  size_t bodyBytes = 0;
  while ((client.connected() || client.available()) && millis() - bodyStarted < API_TIMEOUT_MS && bodyBytes < 2048) {
    while (client.available() && bodyBytes < 2048) {
      Serial.write((uint8_t)client.read()); ++bodyBytes;
    }
    delay(5);
  }
  Serial.println("\nAPI BODY END");
  client.stop();
  const bool ok = statusLine.indexOf(" 200 ") >= 0 || statusLine.indexOf(" 201 ") >= 0;
  Serial.println(ok ? "TEST RESULT: upload accepted; inspect JSON for model_status/coverage/alert_sent" : "TEST RESULT: upload was not accepted; use API STATUS/HEADER/BODY above to locate failure");
  Serial.println("Note: a real image above threshold may send a LINE OA alert.");
  return ok;
}

void pollWebCaptureCommand() {
  if (WiFi.status() != WL_CONNECTED || !cameraReady) return;
  WiFiClientSecure secureClient;
  secureClient.setCACert(ROOT_CA);
  secureClient.setTimeout(10);
  HTTPClient http;
  const String url = String("https://") + API_HOST + "/api/devices/" + DEVICE_ID + "/capture-command";
  if (!http.begin(secureClient, url)) {
    Serial.println("Web command poll: HTTPS setup failed");
    return;
  }
  http.addHeader("X-Device-Token", DEVICE_TOKEN);
  const int code = http.GET();
  if (code < 0) {
    Serial.printf("Web command poll failed: %s\n", HTTPClient::errorToString(code).c_str());
    http.end();
    return;
  }
  const String body = http.getString();
  http.end();
  if (code != 200) {
    Serial.printf("Web command poll returned HTTP %d\n", code);
    return;
  }
  if (!body.startsWith("CAPTURE ")) return;
  String commandId = body.substring(8);
  commandId.trim();
  if (commandId.length() < 10) {
    Serial.println("Web command response had an invalid ID");
    return;
  }
  Serial.printf("WEB COMMAND: capture requested (%s)\n", commandId.c_str());
  captureAndUploadOnce(commandId);
}

void setup() {
  Serial.begin(115200);
  delay(700);
  Serial.println("\n=== DrainGuard ESP32-CAM one-shot test ===");
  Serial.println("No sensors. No periodic capture. No automatic retries.");
  Serial.println("Web control is enabled: keep this board on; the test page can command a capture.");
  Serial.println("Type c + Enter for a local one-shot capture; type i for status.");
  pinMode(FLASH_LED_GPIO_NUM, OUTPUT);
  digitalWrite(FLASH_LED_GPIO_NUM, HIGH);
  Serial.println("FLASH: on while the board is powered");
  cameraReady = setupCamera();
  Serial.printf("CAMERA: %s; PSRAM: %s\n", cameraReady ? "ready" : "failed", psramFound() ? "yes" : "no");
  connectWiFi();
}

void loop() {
  if (Serial.available()) {
    const char command = (char)Serial.read();
    while (Serial.available()) Serial.read();
    if (command == 'c' || command == 'C') captureAndUploadOnce();
    else if (command == 'i' || command == 'I') {
      Serial.printf("CAMERA=%s WIFI=%s RSSI=%d IP=%s FREE_HEAP=%u PSRAM=%u\n",
                    cameraReady ? "ready" : "failed", WiFi.status() == WL_CONNECTED ? "connected" : "disconnected",
                    WiFi.RSSI(), WiFi.localIP().toString().c_str(), ESP.getFreeHeap(), ESP.getFreePsram());
    } else if (command != '\r' && command != '\n') Serial.println("Commands: c=capture/upload once, i=status");
  }
  if (millis() - lastCommandPoll >= COMMAND_POLL_MS) {
    lastCommandPoll = millis();
    pollWebCaptureCommand();
  }
  delay(20);
}
