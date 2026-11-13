#include "esp_camera.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>
#include "esp_sleep.h"
#include "driver/rtc_io.h"
#include "secrets.h"

// Full DrainWatch node: camera + 52 mm float + pulse flow sensor.
// AI Thinker ESP32-CAM, no microSD. Sensors: float GPIO14, flow pulse GPIO13.
static const int FLOAT_PIN = 14;
static const int FLOW_PIN = 13;
static const int FLASH_LED_PIN = 4;
static const int FLOAT_WATER_LEVEL = LOW; // Float closes GPIO14 to GND when water rises.
static const float FLOW_PULSES_PER_LITER = 450.0f; // Calibrate for the actual flow meter.
static const float FLOW_SLOW_LPM = 1.0f;
static const uint32_t FLOW_SAMPLE_MS = 5000;
static const uint32_t TELEMETRY_INTERVAL_MS = 5000;
static const uint32_t COMMAND_INTERVAL_MS = 5000;
static const uint32_t CAPTURE_INTERVAL_MS = 5UL * 60UL * 1000UL;
static const uint32_t NO_WATER_SLEEP_MS = 10UL * 60UL * 1000UL;
static const uint32_t WIFI_TIMEOUT_MS = 30000;

// AI Thinker ESP32-CAM pin map.
static const int PWDN_GPIO_NUM = 32, RESET_GPIO_NUM = -1, XCLK_GPIO_NUM = 0;
static const int SIOD_GPIO_NUM = 26, SIOC_GPIO_NUM = 27;
static const int Y9_GPIO_NUM = 35, Y8_GPIO_NUM = 34, Y7_GPIO_NUM = 39, Y6_GPIO_NUM = 36;
static const int Y5_GPIO_NUM = 21, Y4_GPIO_NUM = 19, Y3_GPIO_NUM = 18, Y2_GPIO_NUM = 5;

static volatile uint32_t flowPulseCount = 0;
static bool cameraReady = false, flashOn = false, lastWaterPresent = false;
static bool sleepGuardInitialized = false, sleepConfirmed = false;
static float flowRateLpm = 0.0f;
static uint32_t lastFlowSample = 0, lastTelemetry = 0, lastCommandPoll = 0, lastSleepCheck = 0, lastGuardReset = 0;
static uint32_t lastCapture = 0, noWaterSince = 0;
static bool hasCapturedSinceWet = false;

void IRAM_ATTR countFlowPulse() { ++flowPulseCount; }

String isoTimestamp() {
  struct tm info;
  if (!getLocalTime(&info, 10000)) return "";
  char text[25];
  strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &info);
  return String(text);
}

void applyFlash(bool enabled) {
  flashOn = enabled;
  digitalWrite(FLASH_LED_PIN, flashOn ? HIGH : LOW);
  Serial.printf("FLASH: %s\n", flashOn ? "ON" : "OFF");
}

void flashBootSignal() {
  Serial.println("BOOT SIGNAL: flash blink 2x");
  for (int i = 0; i < 2; ++i) {
    digitalWrite(FLASH_LED_PIN, HIGH); delay(120);
    digitalWrite(FLASH_LED_PIN, LOW); delay(180);
  }
  flashOn = false;
}

bool connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return true;
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("Connecting Wi-Fi '%s'", WIFI_SSID);
  const uint32_t started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < WIFI_TIMEOUT_MS) {
    delay(300); Serial.print('.');
  }
  Serial.println();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("Wi-Fi failed, status=%d\n", (int)WiFi.status());
    return false;
  }
  Serial.printf("Wi-Fi connected: %s RSSI=%d dBm\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  return true;
}

bool setupCamera() {
  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0; config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM; config.pin_d1 = Y3_GPIO_NUM; config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM; config.pin_d4 = Y6_GPIO_NUM; config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM; config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM; config.pin_pclk = 22; config.pin_vsync = 25;
  config.pin_href = 23; config.pin_sccb_sda = SIOD_GPIO_NUM; config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM; config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000; config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = psramFound() ? FRAMESIZE_SVGA : FRAMESIZE_VGA;
  config.jpeg_quality = 12; config.fb_count = psramFound() ? 2 : 1;
  config.grab_mode = CAMERA_GRAB_LATEST;
  config.fb_location = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
  const esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) Serial.printf("Camera init failed: 0x%x\n", err);
  return err == ESP_OK;
}

void sampleFlow(uint32_t now) {
  if (now - lastFlowSample < FLOW_SAMPLE_MS) return;
  uint32_t pulses;
  noInterrupts(); pulses = flowPulseCount; flowPulseCount = 0; interrupts();
  const float elapsedSeconds = (now - lastFlowSample) / 1000.0f;
  lastFlowSample = now;
  flowRateLpm = elapsedSeconds > 0 ? (pulses / FLOW_PULSES_PER_LITER) * (60.0f / elapsedSeconds) : 0.0f;
  Serial.printf("FLOW: %lu pulses, %.2f L/min\n", (unsigned long)pulses, flowRateLpm);
}

const char *flowStatus() {
  if (flowRateLpm <= 0.01f) return "no_flow";
  return flowRateLpm < FLOW_SLOW_LPM ? "slow" : "ok";
}

String multipartField(const String &boundary, const char *name, const String &value) {
  return "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + String(name) + "\"\r\n\r\n" + value + "\r\n";
}

bool uploadTelemetry(const char *mode) {
  if (!connectWiFi()) return false;
  const String stamp = isoTimestamp();
  if (!stamp.length()) { Serial.println("No NTP time; telemetry skipped"); return false; }
  const bool water = digitalRead(FLOAT_PIN) == FLOAT_WATER_LEVEL;
  const String boundary = "----DGTelemetryBound8b17";
  String body;
  body.reserve(700);
  body += multipartField(boundary, "device_id", DEVICE_ID);
  body += multipartField(boundary, "captured_at", stamp);
  body += multipartField(boundary, "device_token", DEVICE_TOKEN);
  body += multipartField(boundary, "water_level", water ? "1" : "0");
  body += multipartField(boundary, "flow_rate", String(flowRateLpm, 3));
  body += multipartField(boundary, "flow_status", flowStatus());
  body += multipartField(boundary, "device_mode", mode);
  body += multipartField(boundary, "flash_on", flashOn ? "true" : "false");
  body += multipartField(boundary, "camera_ready", cameraReady ? "true" : "false");
  body += "--" + boundary + "--\r\n";

  WiFiClientSecure secure;
  secure.setCACert(ROOT_CA);
  HTTPClient http;
  if (!http.begin(secure, String("https://") + API_HOST + TELEMETRY_PATH)) return false;
  http.setConnectTimeout(12000); http.setTimeout(15000);
  http.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
  const int code = http.POST((uint8_t *)body.c_str(), body.length());
  const String response = code > 0 ? http.getString() : HTTPClient::errorToString(code);
  http.end();
  Serial.printf("TELEMETRY: HTTP %d %s\n", code, response.substring(0, 160).c_str());
  return code >= 200 && code < 300;
}

void pollFlashControl() {
  if (!connectWiFi()) return;
  WiFiClientSecure secure; secure.setCACert(ROOT_CA);
  HTTPClient http;
  String url = String("https://") + API_HOST + "/api/devices/" + DEVICE_ID + "/flash-desired";
  if (!http.begin(secure, url)) return;
  http.setConnectTimeout(10000); http.setTimeout(10000);
  http.addHeader("X-Device-Token", DEVICE_TOKEN);
  const int code = http.GET();
  String desired = code == 200 ? http.getString() : "";
  desired.trim(); http.end();
  if (desired == "ON" && !flashOn) applyFlash(true);
  else if (desired == "OFF" && flashOn) applyFlash(false);
}

bool resetSleepConfirmation() {
  if (!connectWiFi()) return false;
  WiFiClientSecure secure; secure.setCACert(ROOT_CA);
  HTTPClient http;
  const String url = String("https://") + API_HOST + "/api/devices/" + DEVICE_ID + "/sleep-confirmation/reset";
  if (!http.begin(secure, url)) return false;
  http.setConnectTimeout(10000); http.setTimeout(10000);
  http.addHeader("X-Device-Token", DEVICE_TOKEN);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  const int code = http.POST(String("camera_ready=") + (cameraReady ? "true" : "false"));
  const String response = code > 0 ? http.getString() : HTTPClient::errorToString(code);
  http.end();
  if (code >= 200 && code < 300) {
    sleepGuardInitialized = true; sleepConfirmed = false;
    Serial.println("SLEEP GUARD: reset; waiting for dashboard readiness confirmation");
    return true;
  }
  Serial.printf("SLEEP GUARD RESET FAILED: HTTP %d %s\n", code, response.substring(0, 120).c_str());
  return false;
}

void pollSleepConfirmation() {
  if (!sleepGuardInitialized || !connectWiFi()) return;
  WiFiClientSecure secure; secure.setCACert(ROOT_CA);
  HTTPClient http;
  const String url = String("https://") + API_HOST + "/api/devices/" + DEVICE_ID + "/sleep-confirmation";
  if (!http.begin(secure, url)) return;
  http.setConnectTimeout(10000); http.setTimeout(10000);
  http.addHeader("X-Device-Token", DEVICE_TOKEN);
  const int code = http.GET();
  String value = code == 200 ? http.getString() : "";
  http.end(); value.trim();
  if (code != 200) {
    Serial.printf("SLEEP GUARD POLL FAILED: HTTP %d\n", code);
    sleepConfirmed = false;
    return;
  }
  const bool confirmed = value == "READY";
  if (confirmed != sleepConfirmed) {
    Serial.printf("SLEEP GUARD: %s\n", confirmed ? "confirmed; sleep allowed after idle rule" : "not confirmed; sleep blocked");
    if (confirmed) noWaterSince = millis(); // Start a fresh 10-minute idle window after the user confirms readiness.
  }
  sleepConfirmed = confirmed;
}

bool captureAndUpload(const String &commandId = "") {
  if (!cameraReady || !connectWiFi()) return false;
  const String stamp = isoTimestamp();
  if (!stamp.length()) { Serial.println("NTP unavailable; image skipped"); return false; }
  camera_fb_t *frame = esp_camera_fb_get();
  if (!frame || frame->format != PIXFORMAT_JPEG) {
    if (frame) esp_camera_fb_return(frame);
    Serial.println("Camera capture failed"); return false;
  }
  const bool water = digitalRead(FLOAT_PIN) == FLOAT_WATER_LEVEL;
  Serial.printf("CAPTURE: %u bytes, %ux%u, water=%s\n", (unsigned)frame->len, frame->width, frame->height, water ? "yes" : "no");
  WiFiClientSecure client; client.setCACert(ROOT_CA); client.setTimeout(60000);
  if (!client.connect(API_HOST, API_PORT)) {
    esp_camera_fb_return(frame); Serial.println("HTTPS connection failed"); return false;
  }
  const String boundary = "----DGFullSystemCaptureBoundary4b1a";
  String start;
  start.reserve(800);
  start += multipartField(boundary, "device_id", DEVICE_ID);
  start += multipartField(boundary, "captured_at", stamp);
  start += multipartField(boundary, "device_token", DEVICE_TOKEN);
  start += multipartField(boundary, "water_level", water ? "1" : "0");
  start += multipartField(boundary, "flow_rate", String(flowRateLpm, 3));
  start += multipartField(boundary, "flow_status", flowStatus());
  if (commandId.length()) start += multipartField(boundary, "command_id", commandId);
  start += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"image\"; filename=\"drain-camera.jpg\"\r\nContent-Type: image/jpeg\r\n\r\n";
  const String end = "\r\n--" + boundary + "--\r\n";
  const size_t total = start.length() + frame->len + end.length();
  client.print(String("POST ") + API_PATH + " HTTP/1.1\r\nHost: " + API_HOST + "\r\nContent-Type: multipart/form-data; boundary=" + boundary +
               "\r\nContent-Length: " + String(total) + "\r\nConnection: close\r\n\r\n");
  size_t sent = client.write((const uint8_t *)start.c_str(), start.length());
  sent += client.write(frame->buf, frame->len);
  sent += client.write((const uint8_t *)end.c_str(), end.length());
  esp_camera_fb_return(frame);
  if (sent != total) { client.stop(); Serial.printf("Upload incomplete: %u/%u\n", (unsigned)sent, (unsigned)total); return false; }
  // A proxy may send a valid body without a readable status line; do not retry
  // a fully transmitted image or a real water event could create duplicates.
  lastCapture = millis(); hasCapturedSinceWet = true;
  String status = client.readStringUntil('\n'); status.trim();
  Serial.printf("CAPTURE API: %s\n", status.length() ? status.c_str() : "no HTTP status received");
  String response;
  const uint32_t started = millis();
  while ((client.connected() || client.available()) && millis() - started < 60000 && response.length() < 1500) {
    while (client.available() && response.length() < 1500) response += (char)client.read();
    delay(5);
  }
  if (response.length()) Serial.printf("CAPTURE RESPONSE: %s\n", response.c_str());
  client.stop();
  const bool accepted = status.indexOf(" 200 ") >= 0 || status.indexOf(" 201 ") >= 0 ||
                        response.indexOf("\"model_status\"") >= 0;
  return accepted;
}

void pollWebCapture() {
  if (!connectWiFi()) return;
  WiFiClientSecure secure; secure.setCACert(ROOT_CA);
  HTTPClient http;
  const String url = String("https://") + API_HOST + "/api/devices/" + DEVICE_ID + "/capture-command";
  if (!http.begin(secure, url)) return;
  http.setConnectTimeout(10000); http.setTimeout(10000);
  http.addHeader("X-Device-Token", DEVICE_TOKEN);
  const int code = http.GET();
  const String command = code == 200 ? http.getString() : "";
  http.end();
  if (!command.startsWith("CAPTURE ")) return;
  String id = command.substring(8); id.trim();
  if (id.length() > 10) {
    Serial.println("Web capture command received");
    captureAndUpload(id);
  }
}

void enterDeepSleep() {
  if (!sleepGuardInitialized || !sleepConfirmed) {
    Serial.println("Sleep blocked: waiting for dashboard readiness confirmation.");
    return;
  }
  Serial.println("No float water and no flow for 10 minutes; reporting deep sleep.");
  applyFlash(false);
  if (!uploadTelemetry("deep_sleep")) {
    Serial.println("Sleep aborted: backend did not confirm deep-sleep status; retrying while awake.");
    return;
  }
  if (cameraReady) { esp_camera_deinit(); cameraReady = false; }
  WiFi.disconnect(true); WiFi.mode(WIFI_OFF);
  rtc_gpio_init(GPIO_NUM_14);
  rtc_gpio_set_direction(GPIO_NUM_14, RTC_GPIO_MODE_INPUT_ONLY);
  rtc_gpio_pullup_en(GPIO_NUM_14); rtc_gpio_pulldown_dis(GPIO_NUM_14);
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_14, 0); // Float closes to GND when water rises.
  Serial.flush();
  esp_deep_sleep_start();
}

void setup() {
  Serial.begin(115200); delay(300);
  Serial.println("\n=== DrainWatch full ESP32-CAM node ===");
  Serial.println("Float=GPIO14 (active LOW); flow pulse=GPIO13; flash=GPIO4");
  rtc_gpio_deinit(GPIO_NUM_14);
  pinMode(FLOAT_PIN, INPUT_PULLUP); pinMode(FLOW_PIN, INPUT_PULLUP);
  pinMode(FLASH_LED_PIN, OUTPUT); digitalWrite(FLASH_LED_PIN, LOW); flashBootSignal();
  attachInterrupt(digitalPinToInterrupt(FLOW_PIN), countFlowPulse, FALLING);
  if (!setupCamera()) { Serial.println("Camera unavailable; sensor telemetry continues"); }
  else cameraReady = true;
  Serial.printf("CAMERA: %s | PSRAM=%s | FREE_HEAP=%u\n",
                cameraReady ? "ready" : "failed", psramFound() ? "yes" : "no", ESP.getFreeHeap());
  if (connectWiFi()) resetSleepConfirmation();
  else Serial.println("Wi-Fi unavailable; staying awake and retrying; deep sleep remains blocked");
  lastGuardReset = millis();
  lastFlowSample = millis(); lastTelemetry = millis() - TELEMETRY_INTERVAL_MS;
  lastCommandPoll = millis(); lastSleepCheck = millis();
  pollFlashControl();
  const bool water = digitalRead(FLOAT_PIN) == FLOAT_WATER_LEVEL;
  lastWaterPresent = water;
  noWaterSince = millis();
  if (water && cameraReady) captureAndUpload(); // Capture promptly when float wakes the board.
  Serial.printf("Ready; wake cause=%d; water=%s\n", (int)esp_sleep_get_wakeup_cause(), water ? "yes" : "no");
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }
  const uint32_t now = millis();
  if (WiFi.status() == WL_CONNECTED && !sleepGuardInitialized && now - lastGuardReset >= COMMAND_INTERVAL_MS) {
    lastGuardReset = now;
    resetSleepConfirmation();
  }
  sampleFlow(now);
  const bool waterPresent = digitalRead(FLOAT_PIN) == FLOAT_WATER_LEVEL;
  if (waterPresent != lastWaterPresent) {
    Serial.printf("FLOAT: %s\n", waterPresent ? "water detected" : "water absent");
    lastWaterPresent = waterPresent;
    if (waterPresent) { noWaterSince = now; hasCapturedSinceWet = false; }
  }
  if (waterPresent || flowRateLpm > 0.01f) noWaterSince = now;

  if (waterPresent && cameraReady && (!hasCapturedSinceWet || now - lastCapture >= CAPTURE_INTERVAL_MS)) {
    captureAndUpload();
  }
  if (now - lastTelemetry >= TELEMETRY_INTERVAL_MS) {
    lastTelemetry = now;
    uploadTelemetry("active");
  }
  if (now - lastCommandPoll >= COMMAND_INTERVAL_MS) {
    lastCommandPoll = now;
    pollFlashControl();
    pollWebCapture();
  }
  if (now - lastSleepCheck >= COMMAND_INTERVAL_MS) {
    lastSleepCheck = now;
    pollSleepConfirmation();
  }
  if (!waterPresent && flowRateLpm <= 0.01f && now - noWaterSince >= NO_WATER_SLEEP_MS) {
    if (sleepGuardInitialized && sleepConfirmed) enterDeepSleep();
    else if (now - noWaterSince < NO_WATER_SLEEP_MS + COMMAND_INTERVAL_MS) {
      Serial.println("Idle rule met, but deep sleep is blocked until user confirms readiness on dashboard.");
    }
  }
  delay(30);
}
