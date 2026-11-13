#include "esp_camera.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include "esp_sleep.h"
#include "driver/rtc_io.h"
#if __has_include("secrets.h")
#include "secrets.h" // Local file for real credentials; excluded from Git.
#else
#include "secrets.h.example" // Lets the sketch compile before local setup.
#warning "Using placeholder settings. Copy secrets.h.example to secrets.h and set Wi-Fi/API values before connecting the camera."
#endif

// AI Thinker ESP32-CAM pin map. Select "AI Thinker ESP32-CAM" in Arduino IDE.
static const int PWDN_GPIO_NUM = 32, RESET_GPIO_NUM = -1, XCLK_GPIO_NUM = 0;
static const int SIOD_GPIO_NUM = 26, SIOC_GPIO_NUM = 27;
static const int Y9_GPIO_NUM = 35, Y8_GPIO_NUM = 34, Y7_GPIO_NUM = 39, Y6_GPIO_NUM = 36;
static const int Y5_GPIO_NUM = 21, Y4_GPIO_NUM = 19, Y3_GPIO_NUM = 18, Y2_GPIO_NUM = 5;
static const int VSYNC_GPIO_NUM = 25, HREF_GPIO_NUM = 23, PCLK_GPIO_NUM = 22;

// Wi-Fi, API endpoint, device credentials, and TLS root CA live in secrets.h.
const uint32_t CAPTURE_INTERVAL_MS = 5UL * 60UL * 1000UL;
#define ENABLE_FLOW_SENSORS 0 // Set to 1 only after wiring the float and flow sensors.
#if ENABLE_FLOW_SENSORS
// GPIO13: float switch to GND when water raises it. GPIO14: YF-201C pulse output.
// These pins are available only when the microSD slot is not used.
static const int FLOAT_PIN = 13;
static const int FLOW_PIN = 14;
const float FLOW_PULSES_PER_LITER = 450.0f; // Calibrate against a measured volume.
const uint32_t FLOW_SAMPLE_MS = 5000;
volatile uint32_t flowPulses = 0;
void IRAM_ATTR onFlowPulse() { flowPulses++; }
#endif
bool cameraReady = false;

#if ENABLE_FLOW_SENSORS
bool uploadTelemetry() {
  WiFiClientSecure client; client.setCACert(ROOT_CA); client.setTimeout(10000);
  if (WiFi.status() != WL_CONNECTED || !client.connect(API_HOST, API_PORT)) return false;
  String timestamp = isoTimestamp();
  if (!timestamp.length()) { client.stop(); return false; }
  const char *flowStatus = flowPulses == 0 ? "no_flow" : (flowPulses < 5 ? "slow" : "ok");
  String boundary = "----DrainWatchTelemetryBoundary";
  String body = "";
  auto addField = [&](const char *name, String value) {
    body += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + String(name) + "\"\r\n\r\n" + value + "\r\n";
  };
  addField("device_id", DEVICE_ID); addField("captured_at", timestamp);
  addField("device_token", DEVICE_TOKEN);
  addField("water_level", String(digitalRead(FLOAT_PIN) == LOW ? 1 : 0));
  addField("flow_rate", String(flowPulses * 60.0f / FLOW_PULSES_PER_LITER / (FLOW_SAMPLE_MS / 1000.0f), 2));
  addField("flow_status", flowStatus);
  body += "--" + boundary + "--\r\n";
  client.print(String("POST ") + TELEMETRY_PATH + " HTTP/1.1\r\nHost: " + API_HOST + "\r\n" +
               "Content-Type: multipart/form-data; boundary=" + boundary + "\r\n" +
               "Content-Length: " + String(body.length()) + "\r\nConnection: close\r\n\r\n" + body);
  String statusLine = client.readStringUntil('\n'); client.stop();
  return statusLine.indexOf(" 200 ") >= 0;
}
#endif

bool setupCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0; config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM; config.pin_d1 = Y3_GPIO_NUM; config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM; config.pin_d4 = Y6_GPIO_NUM; config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM; config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM; config.pin_pclk = PCLK_GPIO_NUM; config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM; config.pin_sccb_sda = SIOD_GPIO_NUM; config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM; config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000; config.pixel_format = PIXFORMAT_JPEG;
  config.jpeg_quality = 12; config.fb_count = 1;
  config.frame_size = FRAMESIZE_SVGA;
  return esp_camera_init(&config) == ESP_OK;
}

String isoTimestamp() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 5000)) return "";
  char buffer[25]; strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
  return String(buffer);
}

bool uploadFrame(camera_fb_t *frame) {
  if (!frame || frame->format != PIXFORMAT_JPEG) return false;
  WiFiClientSecure client; client.setCACert(ROOT_CA);
  client.setTimeout(15000);
  if (!client.connect(API_HOST, API_PORT)) return false;
  String boundary = "----DrainWatchBoundary7MA4YWxkTrZu0gW";
  String timestamp = isoTimestamp();
  if (timestamp.length() == 0) { client.stop(); return false; }
  String bodyStart = "--" + boundary + "\r\nContent-Disposition: form-data; name=\"device_id\"\r\n\r\n" + String(DEVICE_ID) + "\r\n";
  bodyStart += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"captured_at\"\r\n\r\n" + timestamp + "\r\n";
  bodyStart += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"device_token\"\r\n\r\n" + String(DEVICE_TOKEN) + "\r\n";
#if ENABLE_FLOW_SENSORS
  bodyStart += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"water_level\"\r\n\r\n" + String(digitalRead(FLOAT_PIN) == LOW ? 1 : 0) + "\r\n";
  bodyStart += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"flow_rate\"\r\n\r\n" + String(flowPulses * 60.0f / FLOW_PULSES_PER_LITER / (FLOW_SAMPLE_MS / 1000.0f), 2) + "\r\n";
  const char *flowStatus = flowPulses == 0 ? "no_flow" : (flowPulses < 5 ? "slow" : "ok");
  bodyStart += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"flow_status\"\r\n\r\n" + String(flowStatus) + "\r\n";
#endif
  bodyStart += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"image\"; filename=\"capture.jpg\"\r\nContent-Type: image/jpeg\r\n\r\n";
  String bodyEnd = "\r\n--" + boundary + "--\r\n";
  size_t contentLength = bodyStart.length() + frame->len + bodyEnd.length();
  client.print(String("POST ") + API_PATH + " HTTP/1.1\r\nHost: " + API_HOST + "\r\n" +
               "Content-Type: multipart/form-data; boundary=" + boundary + "\r\n" +
               "Content-Length: " + String(contentLength) + "\r\nConnection: close\r\n\r\n");
  size_t sent = client.write((const uint8_t *)bodyStart.c_str(), bodyStart.length());
  sent += client.write(frame->buf, frame->len);
  sent += client.write((const uint8_t *)bodyEnd.c_str(), bodyEnd.length());
  if (sent != contentLength) { client.stop(); return false; }
  String statusLine = client.readStringUntil('\n');
  client.stop();
  return statusLine.indexOf(" 200 ") >= 0;
}

#if ENABLE_FLOW_SENSORS
uint32_t measureFlowPulses() {
  noInterrupts(); flowPulses = 0; interrupts();
  delay(FLOW_SAMPLE_MS);
  noInterrupts(); uint32_t sampledPulses = flowPulses; interrupts();
  flowPulses = sampledPulses;
  return sampledPulses;
}

void sleepUntilWaterDetected() {
  Serial.println("No flow and float switch normal; entering deep sleep.");
  if (cameraReady) { esp_camera_deinit(); cameraReady = false; }
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  // Float switch closes to GND when raised. GPIO13 is an RTC-capable wake pin.
  rtc_gpio_init(GPIO_NUM_13);
  rtc_gpio_set_direction(GPIO_NUM_13, RTC_GPIO_MODE_INPUT_ONLY);
  rtc_gpio_pullup_en(GPIO_NUM_13);
  rtc_gpio_pulldown_dis(GPIO_NUM_13);
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_13, 0);
  // Periodic wake also checks flow if water passes without lifting the float.
  esp_sleep_enable_timer_wakeup((uint64_t)CAPTURE_INTERVAL_MS * 1000ULL);
  Serial.flush();
  esp_deep_sleep_start();
}
#endif

bool connectWiFi(uint32_t timeoutMs = 20000) {
  WiFi.mode(WIFI_STA); WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  uint32_t started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < timeoutMs) delay(250);
  return WiFi.status() == WL_CONNECTED;
}

void setup() {
  Serial.begin(115200);
#if ENABLE_FLOW_SENSORS
  pinMode(FLOAT_PIN, INPUT_PULLUP);
  pinMode(FLOW_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(FLOW_PIN), onFlowPulse, FALLING);
  uint32_t sampledPulses = measureFlowPulses();
  bool waterHigh = digitalRead(FLOAT_PIN) == LOW;
  Serial.printf("Water level: %s; flow pulses: %lu\n", waterHigh ? "HIGH" : "normal", (unsigned long)sampledPulses);
  if (sampledPulses == 0 && !waterHigh) {
    if (connectWiFi()) { configTime(0, 0, "pool.ntp.org", "time.nist.gov"); uploadTelemetry(); }
    sleepUntilWaterDetected();
  }
#endif

  cameraReady = setupCamera();
  if (!cameraReady) { Serial.println("Camera init failed"); return; }
  if (!connectWiFi()) { Serial.println("Wi-Fi timeout; retry after sleep"); esp_sleep_enable_timer_wakeup(60ULL * 1000000ULL); esp_deep_sleep_start(); }
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    camera_fb_t *frame = esp_camera_fb_get();
    if (frame) { bool ok = uploadFrame(frame); Serial.println(ok ? "Capture uploaded" : "Upload failed"); esp_camera_fb_return(frame); }
  }
  delay(CAPTURE_INTERVAL_MS);
#if ENABLE_FLOW_SENSORS
  uint32_t sampledPulses = measureFlowPulses();
  bool waterHigh = digitalRead(FLOAT_PIN) == LOW;
  Serial.printf("Water level: %s; flow pulses: %lu\n", waterHigh ? "HIGH" : "normal", (unsigned long)sampledPulses);
  if (sampledPulses == 0 && !waterHigh) { uploadTelemetry(); sleepUntilWaterDetected(); }
#endif
}
