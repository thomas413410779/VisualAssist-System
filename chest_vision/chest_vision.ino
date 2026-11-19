/*
 * Blind Assist - Chest Vision Node (DFRobot DFR1154 / ESP32-S3)
 * Endpoints: Port 80 (/capture, /audio, /play, /beep, /status), Port 81 (MJPEG /)
 */

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include "esp_camera.h"
#include "esp_http_server.h"
#include "img_converters.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "driver/i2s.h"

#ifndef WIFI_SSID
const char* WIFI_SSID = "00000000";
#endif
#ifndef WIFI_PASSWORD
const char* WIFI_PASSWORD = "00000001";
#endif
const char* FALLBACK_AP_SSID = "BlindAssist-Camera";
const char* FALLBACK_AP_PASSWORD = "blindassist";

// DFRobot ESP32-S3 AI Camera (SKU: DFR1154, OV3660) Pin Mapping
#define CAMERA_PIN_PWDN   -1
#define CAMERA_PIN_RESET  -1
#define CAMERA_PIN_XCLK   5
#define CAMERA_PIN_SIOD   8
#define CAMERA_PIN_SIOC   9
#define CAMERA_PIN_D7     4   // Y9
#define CAMERA_PIN_D6     6   // Y8
#define CAMERA_PIN_D5     7   // Y7
#define CAMERA_PIN_D4     14  // Y6
#define CAMERA_PIN_D3     17  // Y5
#define CAMERA_PIN_D2     21  // Y4
#define CAMERA_PIN_D1     18  // Y3
#define CAMERA_PIN_D0     16  // Y2
#define CAMERA_PIN_VSYNC  1
#define CAMERA_PIN_HREF   2
#define CAMERA_PIN_PCLK   15

// Audio I2S Pins: PDM Mic (I2S0) & MAX98357 Amp (I2S1)
#define MIC_CLK_PIN       38
#define MIC_DATA_PIN      39
#define SPK_BCLK_PIN      45
#define SPK_LRCLK_PIN     46
#define SPK_DATA_PIN      42

#define AUDIO_SAMPLE_RATE 16000
#define MAX_AUDIO_SEC     6
#define AUDIO_BUFFER_SIZE (AUDIO_SAMPLE_RATE * sizeof(int16_t) * MAX_AUDIO_SEC) // ~192 KB

// IO0 on-board BOOT button
#define TRIGGER_BUTTON_PIN 0

// Layer 0 Hardware Emergency Brake Pins (Gravity: TX=44 -> Trig, RX=43 -> Echo, Alarm=3)
#define ULTRASONIC_TRIG_PIN 44
#define ULTRASONIC_ECHO_PIN 43
#define BRAKE_ALARM_PIN     3
#define EMERGENCY_BRAKE_DIST_CM 80.0f
#define ULTRASONIC_TIMEOUT_US   24000 // ~4m max

static int currentTrigPin = ULTRASONIC_TRIG_PIN;
static int currentEchoPin = ULTRASONIC_ECHO_PIN;
static volatile unsigned long lastSonarDurationUs = 0;
static volatile float lastRawDistCm = 400.0f;
static uint32_t consecutiveSonarTimeouts = 0;
static bool hardwareActive = true;

#define STREAM_BOUNDARY "blind-assist-boundary"
static const char* STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" STREAM_BOUNDARY;
static const char* STREAM_PART = "\r\n--" STREAM_BOUNDARY "\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

httpd_handle_t cameraHttpd = nullptr;
httpd_handle_t streamHttpd = nullptr;
volatile uint32_t triggerCounter = 0;
uint32_t lastButtonChangeMs = 0;

// Layer 0 Fast Path Telemetry
float chestDistanceCm = 400.0f;
bool emergencyBrakeActive = false;
static bool lastBrakeState = false;
uint32_t lastSonarSampleMs = 0;
uint32_t lastAlarmToggleMs = 0;
bool alarmState = false;

// System Diagnostics
bool cameraReady = false;
String cameraModel = "Unknown";
String cameraLastError = "None";
esp_err_t cameraLastErrorCode = ESP_OK;

// Audio State
uint8_t* audioRecordBuffer = nullptr;
volatile uint32_t audioRecordedBytes = 0;
volatile bool isRecording = false;
volatile uint32_t audioCounter = 0;
bool micReady = false;
bool speakerReady = false;
volatile bool audioPlaybackBusy = false;
volatile bool audioPlaybackActive = false;
uint32_t playbackEndTimeMs = 0;
const uint32_t PLAYBACK_COOLDOWN_MS = 3000; // Prevent speaker-to-mic feedback loop
uint32_t buttonPressStartMs = 0;
bool buttonHeld = false;
bool buttonLockout = false;
uint32_t lastDebounceMs = 0;
bool lastRawButtonState = HIGH;
const uint32_t DEBOUNCE_MS = 50;

struct __attribute__((packed)) WavHeader {
  char riff[4] = {'R', 'I', 'F', 'F'};
  uint32_t fileSize;
  char wave[4] = {'W', 'A', 'V', 'E'};
  char fmt[4] = {'f', 'm', 't', ' '};
  uint32_t fmtSize = 16;
  uint16_t audioFormat = 1; // PCM
  uint16_t numChannels = 1; // Mono
  uint32_t sampleRate = AUDIO_SAMPLE_RATE;
  uint32_t byteRate = AUDIO_SAMPLE_RATE * 1 * sizeof(int16_t);
  uint16_t blockAlign = 1 * sizeof(int16_t);
  uint16_t bitsPerSample = 16;
  char data[4] = {'d', 'a', 't', 'a'};
  uint32_t dataSize;
};

// Forward Declarations
static void processAndBoostAudio(uint8_t* buffer, size_t bytes);
static void playTone(uint16_t freq, uint16_t durationMs);

static const char* getCameraErrorDetail(esp_err_t err) {
  switch (err) {
    case ESP_OK: return "OK";
    case ESP_ERR_NOT_FOUND: return "Sensor not found (check FPC cable / SIOD 8 / SIOC 9)";
    case ESP_ERR_NO_MEM: return "Out of memory for framebuffer (verify PSRAM)";
    case ESP_ERR_INVALID_ARG: return "Invalid config parameter";
    case ESP_ERR_INVALID_STATE: return "Invalid camera state";
    case ESP_ERR_TIMEOUT: return "Signal timeout (XCLK 5 / VSYNC / HREF)";
    default: return "Unknown camera error";
  }
}

// CORS whitelist: localhost, local network (RFC 1918), capacitor://
static void addCorsHeaders(httpd_req_t* request) {
  char origin[128] = {0};
  bool allowOrigin = false;

  if (httpd_req_get_hdr_value_str(request, "Origin", origin, sizeof(origin)) == ESP_OK) {
    if (strncmp(origin, "http://localhost", 16) == 0 ||
        strncmp(origin, "https://localhost", 17) == 0 ||
        strncmp(origin, "http://127.0.0.1", 16) == 0 ||
        strncmp(origin, "http://visualassist.local", 25) == 0 ||
        strncmp(origin, "capacitor://", 12) == 0 ||
        strncmp(origin, "http://192.168.", 15) == 0 ||
        strncmp(origin, "https://192.168.", 16) == 0 ||
        strncmp(origin, "http://10.", 10) == 0 ||
        strncmp(origin, "https://10.", 11) == 0 ||
        strncmp(origin, "http://172.", 11) == 0 ||
        strncmp(origin, "https://172.", 12) == 0) {
      allowOrigin = true;
    }
  } else {
    allowOrigin = true;
  }

  if (allowOrigin) {
    httpd_resp_set_hdr(request, "Access-Control-Allow-Origin", origin[0] ? origin : "*");
    if (origin[0]) httpd_resp_set_hdr(request, "Vary", "Origin");
  }

  httpd_resp_set_hdr(request, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  httpd_resp_set_hdr(request, "Access-Control-Allow-Headers", "Content-Type, Range, User-Agent, X-Requested-With");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store, no-cache, must-revalidate");
}

static esp_err_t sendJpegFrame(httpd_req_t* request) {
  addCorsHeaders(request);
  if (!cameraReady) {
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_status(request, "503 Service Unavailable");
    char errJson[256];
    snprintf(errJson, sizeof(errJson), "{\"error\":\"CAMERA_NOT_READY\",\"code\":%d,\"detail\":\"%s\"}",
             static_cast<int>(cameraLastErrorCode), cameraLastError.c_str());
    return httpd_resp_send(request, errJson, HTTPD_RESP_USE_STRLEN);
  }

  camera_fb_t* frame = esp_camera_fb_get();
  if (!frame) {
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_status(request, "500 Internal Server Error");
    return httpd_resp_send(request, "{\"error\":\"FRAME_CAPTURE_FAILED\"}", HTTPD_RESP_USE_STRLEN);
  }

  uint8_t* jpegBuffer = frame->buf;
  size_t jpegLength = frame->len;
  bool converted = false;

  if (frame->format != PIXFORMAT_JPEG) {
    converted = frame2jpg(frame, 82, &jpegBuffer, &jpegLength);
    esp_camera_fb_return(frame);
    frame = nullptr;
    if (!converted) {
      httpd_resp_set_type(request, "application/json; charset=utf-8");
      httpd_resp_set_status(request, "500 Internal Server Error");
      return httpd_resp_send(request, "{\"error\":\"JPEG_CONVERSION_FAILED\"}", HTTPD_RESP_USE_STRLEN);
    }
  }

  esp_err_t result = httpd_resp_send(request, reinterpret_cast<const char*>(jpegBuffer), jpegLength);
  if (frame) esp_camera_fb_return(frame);
  if (converted && jpegBuffer) free(jpegBuffer);
  return result;
}

static esp_err_t streamHandler(httpd_req_t* request) {
  addCorsHeaders(request);
  if (!cameraReady) {
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    char html[1024];
    snprintf(html, sizeof(html),
             "<!DOCTYPE html><html><head><meta charset='utf-8'><title>Camera Status</title>"
             "<style>body{font-family:sans-serif;text-align:center;padding:40px;background:#18181b;color:#f4f4f5;}"
             ".card{background:#27272a;border-radius:12px;padding:24px;display:inline-block;max-width:480px;text-align:left;}"
             "h2{color:#f59e0b;}code{background:#3f3f46;padding:2px 6px;border-radius:4px;color:#38bdf8;}"
             "</style></head><body><div class='card'>"
             "<h2>Camera Not Ready</h2><p>Error: <code>0x%x</code> (%s)</p>"
             "<p>Check FPC cable connection and power supply.</p>"
             "<a href='/' style='color:#38bdf8;'>Retry</a> | <a href='/status' style='color:#38bdf8;'>Telemetry JSON</a>"
             "</div></body></html>",
             static_cast<unsigned int>(cameraLastErrorCode), cameraLastError.c_str());
    return httpd_resp_send(request, html, HTTPD_RESP_USE_STRLEN);
  }

  esp_err_t result = httpd_resp_set_type(request, STREAM_CONTENT_TYPE);
  if (result != ESP_OK) return result;

  char header[128];
  uint32_t streamFailures = 0;
  while (true) {
    camera_fb_t* frame = esp_camera_fb_get();
    if (!frame) {
      if (++streamFailures > 5) break;
      delay(20);
      continue;
    }
    streamFailures = 0;

    uint8_t* jpegBuffer = frame->buf;
    size_t jpegLength = frame->len;
    bool converted = false;
    if (frame->format != PIXFORMAT_JPEG) {
      converted = frame2jpg(frame, 82, &jpegBuffer, &jpegLength);
      esp_camera_fb_return(frame);
      frame = nullptr;
      if (!converted) break;
    }

    size_t headerLength = snprintf(header, sizeof(header), STREAM_PART, jpegLength);
    result = httpd_resp_send_chunk(request, header, headerLength);
    if (result == ESP_OK) result = httpd_resp_send_chunk(request, reinterpret_cast<const char*>(jpegBuffer), jpegLength);
    if (result == ESP_OK) result = httpd_resp_send_chunk(request, "\r\n", 2);

    if (frame) esp_camera_fb_return(frame);
    if (converted && jpegBuffer) free(jpegBuffer);
    if (result != ESP_OK) break;
    delay(20);
  }
  return result;
}

static esp_err_t captureHandler(httpd_req_t* request) {
  addCorsHeaders(request);
  httpd_resp_set_type(request, "image/jpeg");
  return sendJpegFrame(request);
}

// DSP: DC offset removal + adaptive gain + VAD threshold check
static void processAndBoostAudio(uint8_t* buffer, size_t bytes) {
  if (!buffer || bytes < 4) return;
  size_t numSamples = bytes / sizeof(int16_t);
  int16_t* samples = (int16_t*)buffer;

  int64_t sum = 0;
  int64_t sumSquares = 0;
  for (size_t i = 0; i < numSamples; i++) {
    sum += samples[i];
    sumSquares += (int64_t)samples[i] * samples[i];
  }
  int16_t dcOffset = (int16_t)(sum / numSamples);

  double meanSquare = (double)(sumSquares / numSamples) - (double)dcOffset * dcOffset;
  if (meanSquare < 0) meanSquare = 0;
  double rms = sqrt(meanSquare);

  const double VAD_RMS_THRESHOLD = 30.0;
  if (rms < VAD_RMS_THRESHOLD) {
    Serial.printf("[AUDIO] VAD silence (RMS: %.1f < %.1f), skip boost\n", rms, VAD_RMS_THRESHOLD);
    return;
  }

  int gain = (rms > 3000) ? 2 : ((rms > 1000) ? 4 : 8);
  for (size_t i = 0; i < numSamples; i++) {
    int32_t val = (int32_t)(samples[i] - dcOffset) * gain;
    if (val > 32767) val = 32767;
    else if (val < -32768) val = -32768;
    samples[i] = (int16_t)val;
  }
  Serial.printf("[AUDIO] Processed: DC offset=%d, gain=%dx, RMS=%.1f, samples=%u\n",
                dcOffset, gain, rms, (unsigned int)numSamples);
}

static esp_err_t audioHandler(httpd_req_t* request) {
  addCorsHeaders(request);
  if (audioRecordedBytes == 0 || audioRecordBuffer == nullptr) {
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_status(request, "404 Not Found");
    return httpd_resp_send(request, "{\"error\":\"NO_AUDIO_RECORDED\"}", HTTPD_RESP_USE_STRLEN);
  }

  httpd_resp_set_type(request, "audio/wav");
  httpd_resp_set_hdr(request, "Content-Disposition", "inline; filename=\"prompt.wav\"");

  WavHeader header;
  header.dataSize = audioRecordedBytes;
  header.fileSize = sizeof(WavHeader) - 8 + audioRecordedBytes;

  esp_err_t res = httpd_resp_send_chunk(request, reinterpret_cast<const char*>(&header), sizeof(WavHeader));
  if (res == ESP_OK) {
    const size_t chunkSize = 2048;
    size_t sent = 0;
    while (sent < audioRecordedBytes) {
      size_t toSend = min(chunkSize, (size_t)(audioRecordedBytes - sent));
      res = httpd_resp_send_chunk(request, reinterpret_cast<const char*>(audioRecordBuffer + sent), toSend);
      if (res != ESP_OK) break;
      sent += toSend;
    }
  }
  if (res == ESP_OK) httpd_resp_send_chunk(request, nullptr, 0);
  return res;
}

// Direct sine synth for MAX98357
static void playTone(uint16_t freq, uint16_t durationMs) {
  if (!speakerReady || audioPlaybackBusy) return;
  audioPlaybackBusy = true;
  i2s_zero_dma_buffer(I2S_NUM_1);

  const uint32_t sampleRate = AUDIO_SAMPLE_RATE;
  const size_t totalSamples = (sampleRate * durationMs) / 1000;
  int16_t buffer[256 * 2];
  float phase = 0.0f;
  const float phaseInc = 2.0f * 3.14159265f * freq / sampleRate;

  size_t samplesRemaining = totalSamples;
  while (samplesRemaining > 0) {
    size_t chunk = min((size_t)256, samplesRemaining);
    for (size_t i = 0; i < chunk; i++) {
      int16_t sample = (int16_t)(sinf(phase) * 16000.0f);
      phase += phaseInc;
      if (phase >= 2.0f * 3.14159265f) phase -= 2.0f * 3.14159265f;
      buffer[i * 2] = sample;     // Left
      buffer[i * 2 + 1] = sample; // Right
    }
    size_t bytesWritten = 0;
    i2s_write(I2S_NUM_1, buffer, chunk * sizeof(int16_t) * 2, &bytesWritten, portMAX_DELAY);
    samplesRemaining -= chunk;
  }
  audioPlaybackBusy = false;
}

static void playStartupChime() {
  playTone(523, 100); // C5
  delay(20);
  playTone(659, 100); // E5
  delay(20);
  playTone(784, 180); // G5
}

static void playAlertTone() {
  playTone(1200, 80);
  delay(30);
  playTone(1600, 120);
}

static esp_err_t beepHandler(httpd_req_t* request) {
  addCorsHeaders(request);
  playAlertTone();
  httpd_resp_set_type(request, "application/json; charset=utf-8");
  return httpd_resp_send(request, "{\"status\":\"BEEP_OK\"}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t playHandler(httpd_req_t* request) {
  addCorsHeaders(request);
  if (!speakerReady) {
    httpd_resp_set_status(request, "503 Service Unavailable");
    return httpd_resp_send(request, "{\"error\":\"SPEAKER_NOT_READY\"}", HTTPD_RESP_USE_STRLEN);
  }

  int totalLen = request->content_len;
  if (totalLen <= 0) {
    httpd_resp_set_status(request, "400 Bad Request");
    return httpd_resp_send(request, "{\"error\":\"EMPTY_BODY\"}", HTTPD_RESP_USE_STRLEN);
  }
  if (audioPlaybackBusy) {
    httpd_resp_set_status(request, "409 Conflict");
    return httpd_resp_send(request, "{\"error\":\"AUDIO_PLAYBACK_BUSY\"}", HTTPD_RESP_USE_STRLEN);
  }

  audioPlaybackActive = true;
  audioPlaybackBusy = true;
  i2s_zero_dma_buffer(I2S_NUM_1);

  char buf[1024];
  int curLen = 0;
  uint8_t wavHeader[44] = {};
  size_t headerBytes = 0;
  bool headerParsed = false;

  while (curLen < totalLen) {
    int received = httpd_req_recv(request, buf, sizeof(buf));
    if (received <= 0) {
      if (received == HTTPD_SOCK_ERR_TIMEOUT) continue;
      break;
    }
    curLen += received;

    uint8_t* playData = reinterpret_cast<uint8_t*>(buf);
    size_t playLen = static_cast<size_t>(received);

    // Accumulate full 44-byte WAV header before sending audio frames to I2S
    if (!headerParsed) {
      const size_t needed = sizeof(wavHeader) - headerBytes;
      const size_t copyLen = min(needed, playLen);
      memcpy(wavHeader + headerBytes, playData, copyLen);
      headerBytes += copyLen;
      playData += copyLen;
      playLen -= copyLen;
      if (headerBytes < sizeof(wavHeader)) continue;

      const bool validWav = (memcmp(wavHeader, "RIFF", 4) == 0 && memcmp(wavHeader + 8, "WAVE", 4) == 0);
      if (!validWav) {
        audioPlaybackBusy = false;
        audioPlaybackActive = false;
        httpd_resp_set_status(request, "415 Unsupported Media Type");
        return httpd_resp_send(request, "{\"error\":\"INVALID_WAV\"}", HTTPD_RESP_USE_STRLEN);
      }
      headerParsed = true;
    }

    if (playLen > 0) {
      playLen -= playLen % (sizeof(int16_t) * 2); // Maintain 4-byte stereo frame alignment
      if (playLen == 0) continue;
      size_t bytesWritten = 0;
      i2s_write(I2S_NUM_1, playData, playLen, &bytesWritten, portMAX_DELAY);
    }
  }

  audioPlaybackBusy = false;
  audioPlaybackActive = false;
  playbackEndTimeMs = millis();

  httpd_resp_set_type(request, "application/json; charset=utf-8");
  return httpd_resp_send(request, "{\"status\":\"PLAY_COMPLETED\"}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t optionsHandler(httpd_req_t* request) {
  addCorsHeaders(request);
  httpd_resp_set_status(request, "200 OK");
  return httpd_resp_send(request, nullptr, 0);
}

static esp_err_t statusHandler(httpd_req_t* request) {
  addCorsHeaders(request);
  httpd_resp_set_type(request, "application/json; charset=utf-8");

  const bool inApMode = (WiFi.getMode() == WIFI_AP);
  const char* currentSsid = inApMode ? FALLBACK_AP_SSID : WIFI_SSID;
  const int currentRssi = inApMode ? 0 : WiFi.RSSI();
  const uint32_t freeHeapKb = ESP.getFreeHeap() / 1024;
  const uint32_t uptimeSec = millis() / 1000;
  const uint32_t psramTotalKb = psramFound() ? (ESP.getPsramSize() / 1024) : 0;
  const uint32_t psramFreeKb = psramFound() ? (ESP.getFreePsram() / 1024) : 0;

  char payload[576];
  snprintf(payload, sizeof(payload),
           "{\"triggerCounter\":%lu,\"audioCounter\":%lu,\"isRecording\":%s,"
           "\"micReady\":%s,\"speakerReady\":%s,\"audioBytes\":%lu,"
           "\"ip\":\"%s\",\"ultrasonicCm\":%.1f,\"emergencyBrake\":%s,"
           "\"sonarDurationUs\":%lu,\"trigPin\":%d,\"echoPin\":%d,"
           "\"cameraReady\":%s,\"cameraModel\":\"%s\",\"cameraError\":\"%s\","
           "\"wifiMode\":\"%s\",\"wifiSsid\":\"%s\",\"wifiRssi\":%d,"
           "\"psramFound\":%s,\"psramTotalKb\":%lu,\"psramFreeKb\":%lu,"
           "\"freeHeapKb\":%lu,\"uptimeSec\":%lu}",
           static_cast<unsigned long>(triggerCounter),
           static_cast<unsigned long>(audioCounter),
           isRecording ? "true" : "false",
           micReady ? "true" : "false",
           speakerReady ? "true" : "false",
           static_cast<unsigned long>(audioRecordedBytes),
           inApMode ? WiFi.softAPIP().toString().c_str() : WiFi.localIP().toString().c_str(),
           chestDistanceCm,
           emergencyBrakeActive ? "true" : "false",
           static_cast<unsigned long>(lastSonarDurationUs),
           currentTrigPin,
           currentEchoPin,
           cameraReady ? "true" : "false",
           cameraModel.c_str(),
           cameraLastError.c_str(),
           inApMode ? "AP" : "STA",
           currentSsid,
           currentRssi,
           psramFound() ? "true" : "false",
           static_cast<unsigned long>(psramTotalKb),
           static_cast<unsigned long>(psramFreeKb),
           static_cast<unsigned long>(freeHeapKb),
           static_cast<unsigned long>(uptimeSec));

  return httpd_resp_send(request, payload, HTTPD_RESP_USE_STRLEN);
}

static void startCameraServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.ctrl_port = 32768;
  config.max_uri_handlers = 10;
  config.max_open_sockets = 7;
  config.lru_purge_enable = true;

  httpd_uri_t captureUri = { .uri = "/capture", .method = HTTP_GET, .handler = captureHandler, .user_ctx = nullptr };
  httpd_uri_t audioUri = { .uri = "/audio", .method = HTTP_GET, .handler = audioHandler, .user_ctx = nullptr };
  httpd_uri_t playUri = { .uri = "/play", .method = HTTP_POST, .handler = playHandler, .user_ctx = nullptr };
  httpd_uri_t playOptionsUri = { .uri = "/play", .method = HTTP_OPTIONS, .handler = optionsHandler, .user_ctx = nullptr };
  httpd_uri_t beepUri = { .uri = "/beep", .method = HTTP_GET, .handler = beepHandler, .user_ctx = nullptr };
  httpd_uri_t statusUri = { .uri = "/status", .method = HTTP_GET, .handler = statusHandler, .user_ctx = nullptr };

  if (httpd_start(&cameraHttpd, &config) == ESP_OK) {
    httpd_register_uri_handler(cameraHttpd, &captureUri);
    httpd_register_uri_handler(cameraHttpd, &audioUri);
    httpd_register_uri_handler(cameraHttpd, &playUri);
    httpd_register_uri_handler(cameraHttpd, &playOptionsUri);
    httpd_register_uri_handler(cameraHttpd, &beepUri);
    httpd_register_uri_handler(cameraHttpd, &statusUri);
    Serial.println("[HTTP] Port 80 ready (/capture, /audio, /play, /beep, /status)");
  }

  httpd_config_t streamConfig = HTTPD_DEFAULT_CONFIG();
  streamConfig.server_port = 81;
  streamConfig.ctrl_port = 32769;
  streamConfig.max_uri_handlers = 2;
  streamConfig.max_open_sockets = 4;
  streamConfig.lru_purge_enable = true;

  httpd_uri_t streamUri = { .uri = "/", .method = HTTP_GET, .handler = streamHandler, .user_ctx = nullptr };
  if (httpd_start(&streamHttpd, &streamConfig) == ESP_OK) {
    httpd_register_uri_handler(streamHttpd, &streamUri);
    Serial.println("[HTTP] Port 81 MJPEG stream ready");
  }
}

static bool initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = CAMERA_PIN_D0;
  config.pin_d1 = CAMERA_PIN_D1;
  config.pin_d2 = CAMERA_PIN_D2;
  config.pin_d3 = CAMERA_PIN_D3;
  config.pin_d4 = CAMERA_PIN_D4;
  config.pin_d5 = CAMERA_PIN_D5;
  config.pin_d6 = CAMERA_PIN_D6;
  config.pin_d7 = CAMERA_PIN_D7;
  config.pin_xclk = CAMERA_PIN_XCLK;
  config.pin_pclk = CAMERA_PIN_PCLK;
  config.pin_vsync = CAMERA_PIN_VSYNC;
  config.pin_href = CAMERA_PIN_HREF;
  config.pin_sccb_sda = CAMERA_PIN_SIOD;
  config.pin_sccb_scl = CAMERA_PIN_SIOC;
  config.pin_pwdn = CAMERA_PIN_PWDN;
  config.pin_reset = CAMERA_PIN_RESET;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_VGA;
  config.jpeg_quality = 12;
  config.fb_count = psramFound() ? 2 : 1;
  config.grab_mode = CAMERA_GRAB_LATEST;
  config.fb_location = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;

  cameraLastErrorCode = esp_camera_init(&config);
  if (cameraLastErrorCode != ESP_OK) {
    cameraLastError = getCameraErrorDetail(cameraLastErrorCode);
    Serial.printf("[CAMERA ERROR] Init failed (0x%x): %s\n", cameraLastErrorCode, cameraLastError.c_str());
    cameraReady = false;
    return false;
  }

  sensor_t* sensor = esp_camera_sensor_get();
  if (sensor) {
    if (sensor->id.PID == OV3660_PID) {
      cameraModel = "OV3660 (3MP Wide-Angle)";
      sensor->set_vflip(sensor, 1);
      sensor->set_brightness(sensor, 1);
      sensor->set_saturation(sensor, -1);
    } else if (sensor->id.PID == OV2640_PID) {
      cameraModel = "OV2640 (2MP)";
    } else {
      cameraModel = "PID:0x" + String(sensor->id.PID, HEX);
    }
  }

  // Flush 2 warm-up frames for auto-exposure stabilization
  for (int i = 0; i < 2; i++) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb) esp_camera_fb_return(fb);
    delay(40);
  }

  cameraReady = true;
  cameraLastError = "None";
  Serial.printf("[CAMERA] Ready (%s, VGA, PSRAM double-buffer)\n", cameraModel.c_str());
  return true;
}

static void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("[WIFI] Connecting to '%s'...\n", WIFI_SSID);

  const uint32_t deadline = millis() + 15000;
  while (WiFi.status() != WL_CONNECTED && millis() < deadline) {
    delay(500);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[WIFI] Connected: http://%s (RSSI: %d dBm)\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
    if (MDNS.begin("visualassist")) {
      MDNS.addService("http", "tcp", 80);
      MDNS.addService("mjpeg", "tcp", 81);
      Serial.println("[MDNS] http://visualassist.local");
    }
    return;
  }

  Serial.println("[WIFI] STA timeout. Starting fallback AP mode...");
  WiFi.mode(WIFI_AP);
  if (WiFi.softAP(FALLBACK_AP_SSID, FALLBACK_AP_PASSWORD)) {
    Serial.printf("[WIFI] Fallback AP active: %s (IP: http://%s)\n", FALLBACK_AP_SSID, WiFi.softAPIP().toString().c_str());
    if (MDNS.begin("visualassist")) {
      MDNS.addService("http", "tcp", 80);
      MDNS.addService("mjpeg", "tcp", 81);
    }
  } else {
    Serial.println("[WIFI ERROR] Failed to start fallback AP");
  }
}

static bool initAudio() {
  audioRecordBuffer = (uint8_t*)(psramFound() ? ps_malloc(AUDIO_BUFFER_SIZE) : malloc(AUDIO_BUFFER_SIZE));
  if (!audioRecordBuffer) {
    Serial.println("[AUDIO ERROR] Failed to allocate audio buffer");
  }

  // I2S0: PDM Mic (RX)
  i2s_config_t mic_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_PDM),
    .sample_rate = AUDIO_SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 4,
    .dma_buf_len = 512,
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
  };
  i2s_pin_config_t mic_pins = {
    .bck_io_num = I2S_PIN_NO_CHANGE,
    .ws_io_num = MIC_CLK_PIN,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = MIC_DATA_PIN
  };
  if (i2s_driver_install(I2S_NUM_0, &mic_config, 0, NULL) == ESP_OK) {
    i2s_set_pin(I2S_NUM_0, &mic_pins);
    micReady = true;
    Serial.println("[AUDIO] PDM Mic ready (16kHz 16-bit Mono, CLK:38, DATA:39)");
  }

  // I2S1: MAX98357 Amp (TX)
  i2s_config_t spk_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = AUDIO_SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 4,
    .dma_buf_len = 512,
    .use_apll = false,
    .tx_desc_auto_clear = true,
    .fixed_mclk = 0
  };
  i2s_pin_config_t spk_pins = {
    .bck_io_num = SPK_BCLK_PIN,
    .ws_io_num = SPK_LRCLK_PIN,
    .data_out_num = SPK_DATA_PIN,
    .data_in_num = I2S_PIN_NO_CHANGE
  };
  if (i2s_driver_install(I2S_NUM_1, &spk_config, 0, NULL) == ESP_OK) {
    i2s_set_pin(I2S_NUM_1, &spk_pins);
    speakerReady = true;
    Serial.println("[AUDIO] MAX98357 Amp ready (BCLK:45, LRCLK:46, DATA:42)");
  }

  return (micReady || speakerReady);
}

static float readUltrasonicDistanceCm() {
  digitalWrite(currentTrigPin, LOW);
  delayMicroseconds(2);
  digitalWrite(currentTrigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(currentTrigPin, LOW);

  const unsigned long duration = pulseIn(currentEchoPin, HIGH, ULTRASONIC_TIMEOUT_US);
  lastSonarDurationUs = duration;
  if (duration == 0) {
    consecutiveSonarTimeouts++;
    lastRawDistCm = 400.0f;
    return 400.0f;
  }
  consecutiveSonarTimeouts = 0;
  const float dist = duration * 0.0343f / 2.0f;
  const float validDist = (dist > 1.0f && dist < 450.0f) ? dist : 400.0f;
  lastRawDistCm = validDist;
  return validDist;
}

static void updateLayer0EmergencyBrake() {
  const uint32_t now = millis();
  // 25ms sampling (< 30ms reflexive latency)
  if (now - lastSonarSampleMs >= 25) {
    lastSonarSampleMs = now;
    const float rawDist = readUltrasonicDistanceCm();
    chestDistanceCm = (rawDist < chestDistanceCm) ? rawDist : (0.7f * chestDistanceCm + 0.3f * rawDist);
    emergencyBrakeActive = (chestDistanceCm < EMERGENCY_BRAKE_DIST_CM);

    // Event-driven state change log (no polling spam)
    if (emergencyBrakeActive != lastBrakeState) {
      lastBrakeState = emergencyBrakeActive;
      if (emergencyBrakeActive) {
        Serial.printf("[BRAKE] 🚨 Emergency brake triggered: %.1f cm (< 80cm)\n", chestDistanceCm);
      } else {
        Serial.printf("[BRAKE] Emergency brake cleared: %.1f cm\n", chestDistanceCm);
      }
    }
  }

  if (emergencyBrakeActive) {
    if (now - lastAlarmToggleMs >= 35) {
      lastAlarmToggleMs = now;
      alarmState = !alarmState;
      digitalWrite(BRAKE_ALARM_PIN, alarmState ? HIGH : LOW);
    }
  } else if (!isRecording) {
    alarmState = false;
    digitalWrite(BRAKE_ALARM_PIN, LOW);
  }
}

void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
  Serial.begin(115200);
  delay(100);

  Serial.println("[INIT] Blind Assist - Chest Vision Node (DFRobot DFR1154)");
  if (psramFound()) {
    Serial.printf("[INFO] PSRAM: %u KB (Free: %u KB), Heap: %u KB\n",
                  ESP.getPsramSize() / 1024, ESP.getFreePsram() / 1024, ESP.getFreeHeap() / 1024);
  }

  pinMode(TRIGGER_BUTTON_PIN, INPUT_PULLUP);
  gpio_pullup_en(GPIO_NUM_0);
  gpio_pulldown_dis(GPIO_NUM_0);

  pinMode(currentTrigPin, OUTPUT);
  pinMode(currentEchoPin, INPUT);
  pinMode(BRAKE_ALARM_PIN, OUTPUT);
  digitalWrite(currentTrigPin, LOW);
  digitalWrite(BRAKE_ALARM_PIN, LOW);

  initCamera();
  initAudio();
  connectWiFi();
  startCameraServer();
  playStartupChime();

  Serial.printf("[READY] Trig:%d, Echo:%d, Alarm:%d. Type 'help' for commands.\n",
                currentTrigPin, currentEchoPin, BRAKE_ALARM_PIN);
}

void loop() {
  if (hardwareActive) {
    updateLayer0EmergencyBrake();
  }

  const uint32_t now = millis();

  // Button debouncing
  const bool rawPressed = (digitalRead(TRIGGER_BUTTON_PIN) == LOW);
  if (rawPressed != lastRawButtonState) {
    lastDebounceMs = now;
  }
  bool pressed = (now - lastDebounceMs > DEBOUNCE_MS) ? rawPressed : false;
  lastRawButtonState = rawPressed;

  // Interactive serial commands
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd.equalsIgnoreCase("swap") || cmd.equalsIgnoreCase("s")) {
      pinMode(currentTrigPin, INPUT);
      pinMode(currentEchoPin, INPUT);
      int tmp = currentTrigPin;
      currentTrigPin = currentEchoPin;
      currentEchoPin = tmp;
      pinMode(currentTrigPin, OUTPUT);
      pinMode(currentEchoPin, INPUT);
      digitalWrite(currentTrigPin, LOW);
      Serial.printf("[CMD] Swapped: Trig=GPIO%d, Echo=GPIO%d\n", currentTrigPin, currentEchoPin);
    } else if (cmd.equalsIgnoreCase("stop") || cmd.equalsIgnoreCase("pause")) {
      hardwareActive = false;
      digitalWrite(currentTrigPin, LOW);
      digitalWrite(BRAKE_ALARM_PIN, LOW);
      Serial.println("[CMD] Hardware sonar & brake paused. 'start' to resume.");
    } else if (cmd.equalsIgnoreCase("start") || cmd.equalsIgnoreCase("resume")) {
      hardwareActive = true;
      Serial.println("[CMD] Hardware sonar & brake resumed.");
    } else if (cmd.equalsIgnoreCase("status")) {
      Serial.printf("[STATUS] Dist: %.1f cm (Raw: %.1f, Duration: %lu us, Timeouts: %lu), Brake: %s, Trig: %d, Echo: %d\n",
                    chestDistanceCm, lastRawDistCm, static_cast<unsigned long>(lastSonarDurationUs),
                    static_cast<unsigned long>(consecutiveSonarTimeouts),
                    emergencyBrakeActive ? "ACTIVE" : "OK", currentTrigPin, currentEchoPin);
    } else if (cmd.equalsIgnoreCase("off") || cmd.equalsIgnoreCase("sleep")) {
      Serial.println("[CMD] Entering deep sleep...");
      delay(100);
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      digitalWrite(currentTrigPin, LOW);
      digitalWrite(BRAKE_ALARM_PIN, LOW);
      esp_deep_sleep_start();
    } else if (cmd.startsWith("pins ")) {
      int t = -1, e = -1;
      if (sscanf(cmd.c_str(), "pins %d %d", &t, &e) == 2 && t >= 0 && e >= 0) {
        pinMode(currentTrigPin, INPUT);
        pinMode(currentEchoPin, INPUT);
        currentTrigPin = t;
        currentEchoPin = e;
        pinMode(currentTrigPin, OUTPUT);
        pinMode(currentEchoPin, INPUT);
        digitalWrite(currentTrigPin, LOW);
        Serial.printf("[CMD] Pins set: Trig=GPIO%d, Echo=GPIO%d\n", currentTrigPin, currentEchoPin);
      }
    } else if (cmd.equalsIgnoreCase("help")) {
      Serial.println("[HELP] Commands: swap | stop | start | status | pins <t> <e> | sleep");
    }
  }

  // Button release unlocking
  if (!pressed && buttonLockout) {
    buttonLockout = false;
  }

  // Button pressed handling
  if (pressed) {
    if (!buttonLockout) {
      if (!buttonHeld) {
        buttonHeld = true;
        buttonPressStartMs = now;
      } else {
        const uint32_t holdTime = now - buttonPressStartMs;

        // Long press (> 350ms): push-to-talk recording
        if (!isRecording && holdTime > 350) {
          if (!audioPlaybackActive && (playbackEndTimeMs == 0 || millis() - playbackEndTimeMs >= PLAYBACK_COOLDOWN_MS)) {
            isRecording = true;
            audioRecordedBytes = 0;
            if (micReady) {
              i2s_zero_dma_buffer(I2S_NUM_0);
              i2s_start(I2S_NUM_0);
            }
            digitalWrite(BRAKE_ALARM_PIN, HIGH);
            playTone(800, 70);
            Serial.println("[AUDIO] Recording started...");
          }
        }

        // Active recording stream from I2S0 PDM mic
        if (isRecording && audioRecordBuffer && micReady) {
          if (audioPlaybackActive) {
            isRecording = false;
            digitalWrite(BRAKE_ALARM_PIN, LOW);
            i2s_stop(I2S_NUM_0);
          } else if (audioRecordedBytes < AUDIO_BUFFER_SIZE) {
            size_t bytesRead = 0;
            size_t bytesToRead = min((size_t)1024, (size_t)(AUDIO_BUFFER_SIZE - audioRecordedBytes));
            esp_err_t r = i2s_read(I2S_NUM_0, audioRecordBuffer + audioRecordedBytes, bytesToRead, &bytesRead, pdMS_TO_TICKS(10));
            if (r == ESP_OK && bytesRead > 0) {
              audioRecordedBytes += bytesRead;
            }
          } else {
            // Max 6s buffer reached
            isRecording = false;
            digitalWrite(BRAKE_ALARM_PIN, LOW);
            i2s_stop(I2S_NUM_0);
            processAndBoostAudio(audioRecordBuffer, audioRecordedBytes);
            playTone(1500, 80);
            audioCounter++;
            triggerCounter++;
            Serial.printf("[AUDIO] Max 6s reached (%lu bytes). Auto-triggered photo.\n",
                          static_cast<unsigned long>(audioRecordedBytes));
          }
        }

        // Safety lockout: hold > 7s prevents infinite loops if floating
        if (holdTime > 7000) {
          if (isRecording) {
            isRecording = false;
            digitalWrite(BRAKE_ALARM_PIN, LOW);
            if (micReady) i2s_stop(I2S_NUM_0);
            processAndBoostAudio(audioRecordBuffer, audioRecordedBytes);
            playTone(1500, 80);
            audioCounter++;
            triggerCounter++;
          }
          buttonHeld = false;
          buttonLockout = true;
          Serial.println("[BUTTON] 7s hold safety lockout engaged");
        }
      }
    }
  } else {
    // Button release handling
    if (buttonHeld) {
      const uint32_t pressDuration = now - buttonPressStartMs;
      if (isRecording) {
        isRecording = false;
        digitalWrite(BRAKE_ALARM_PIN, LOW);
        if (micReady) i2s_stop(I2S_NUM_0);
        processAndBoostAudio(audioRecordBuffer, audioRecordedBytes);
        playTone(1500, 80);
        audioCounter++;
        triggerCounter++;
        Serial.printf("[AUDIO] Recorded %lu bytes. Photo question triggered.\n",
                      static_cast<unsigned long>(audioRecordedBytes));
      } else if (pressDuration > 50 && (now - lastButtonChangeMs > 700)) {
        lastButtonChangeMs = now;
        triggerCounter++;
        playTone(1200, 50);
        Serial.printf("[TRIGGER] Shutter clicked (%lu)\n", static_cast<unsigned long>(triggerCounter));
      }
      buttonHeld = false;
    }
  }

  delay(5);
}
