/*
 * Blind Assist - Chest Vision Node
 * Target: Espressif ESP32-S3-EYE v2.1/v2.2 with Arduino-ESP32 camera driver.
 *
 * Fast path: expose a local MJPEG stream for the phone.
 * Slow path: expose /capture for an on-demand JPEG and /status for a physical-button event.
 * The phone is responsible for sending the JPEG to the cloud model and speaking the result.
 *
 * IMPORTANT:
 * - This sketch assumes the official ESP32-S3-EYE pin map documented by Espressif.
 * - GPIO0 is the on-board BOOT button. Do not hold it while resetting/flashing.
 * - For a custom ESP32-S3 + OV2640 board, replace the CAMERA_* constants with that board's map.
 */

#include <Arduino.h>
#include <WiFi.h>
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

// DFRobot ESP32-S3 AI Camera (SKU: DFR1154, OV3660) Camera Pin Mapping
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

// ---------- DFRobot 音訊硬體配置 ----------
// 1. 板載 I2S PDM 數位麥克風 (I2S_NUM_0)
#define MIC_CLK_PIN       38
#define MIC_DATA_PIN      39
// 2. 板載 MAX98357 擴大機與喇叭接口 (I2S_NUM_1)
#define SPK_BCLK_PIN      45
#define SPK_LRCLK_PIN     46
#define SPK_DATA_PIN      42

#define AUDIO_SAMPLE_RATE 16000
#define MAX_AUDIO_SEC     6
#define AUDIO_BUFFER_SIZE (AUDIO_SAMPLE_RATE * sizeof(int16_t) * MAX_AUDIO_SEC) // ~192 KB

// Official on-board BOOT button.
#define TRIGGER_BUTTON_PIN 0

// ---------- Layer 0: Hardware Emergency Brake Pins ----------
// DFRobot Gravity Port: IO43 (RX), IO44 (TX)
#define ULTRASONIC_TRIG_PIN 43
#define ULTRASONIC_ECHO_PIN 44
#define BRAKE_ALARM_PIN     3   // 板載指示燈 (IO3) 同步警報指示
#define EMERGENCY_BRAKE_DIST_CM 80.0f
#define ULTRASONIC_TIMEOUT_US 24000 // ~4 meters max

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
uint32_t lastSonarSampleMs = 0;
uint32_t lastAlarmToggleMs = 0;
bool alarmState = false;

// ---------- 系統狀態與診斷紀錄 ----------
bool cameraReady = false;
String cameraModel = "Unknown";
String cameraLastError = "None";
esp_err_t cameraLastErrorCode = ESP_OK;

// ---------- 語音錄音與播放狀態 (DFRobot PDM Mic & Speaker) ----------
uint8_t* audioRecordBuffer = nullptr;
volatile uint32_t audioRecordedBytes = 0;
volatile bool isRecording = false;
volatile uint32_t audioCounter = 0;
bool micReady = false;
bool speakerReady = false;
volatile bool audioPlaybackBusy = false;
volatile bool audioPlaybackActive = false;
uint32_t playbackEndTimeMs = 0;
const uint32_t PLAYBACK_COOLDOWN_MS = 3000; // 喇叭播放結束後 3 秒內禁止錄音，防止回饋循環
uint32_t buttonPressStartMs = 0;
bool buttonHeld = false;
bool buttonLockout = false; // 7秒安全機制觸發後鎖定，直到按鈕真正放開
uint32_t lastDebounceMs = 0;
bool lastRawButtonState = HIGH;
const uint32_t DEBOUNCE_MS = 50; // 按鍵消抖時間

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

// 將 esp_err_t 相機錯誤代碼翻譯為易懂的中文診斷訊息
const char* getCameraErrorDetail(esp_err_t err) {
  switch (err) {
    case ESP_OK:
      return "正常 (OK)";
    case ESP_ERR_NOT_FOUND:
      return "0x105 (ESP_ERR_NOT_FOUND): 找不到相機感測器！請檢查鏡頭排線是否插緊扣牢，或確認 SIOD(8)/SIOC(9) 控制腳位。";
    case ESP_ERR_NO_MEM:
      return "0x101 (ESP_ERR_NO_MEM): 記憶體不足！無法配置相機 Framebuffer，請確認 OPI PSRAM 是否正常啟用。";
    case ESP_ERR_INVALID_ARG:
      return "0x102 (ESP_ERR_INVALID_ARG): 初始化參數無效！請檢查解析度 (frame_size) 或像素格式設定。";
    case ESP_ERR_INVALID_STATE:
      return "0x103 (ESP_ERR_INVALID_STATE): 相機狀態無效或已處於初始化狀態。";
    case ESP_ERR_TIMEOUT:
      return "0x107 (ESP_ERR_TIMEOUT): 相機訊號等待超時！XCLK (5) 或 VSYNC/HREF 同步訊號無回應。";
    default:
      return "未知的相機硬體錯誤代碼。";
  }
}

// 針對區域網路與本地 Web/手機客戶端進行安全 CORS 檢查
static void addCorsHeaders(httpd_req_t* request) {
  char origin[128] = {0};
  bool allowOrigin = false;

  // 取得請求發起端之 Origin Header
  if (httpd_req_get_hdr_value_str(request, "Origin", origin, sizeof(origin)) == ESP_OK) {
    // 白名單校驗：僅允許 localhost、Capacitor 原生應用程式協議與私人區域網路 IP (RFC 1918)
    if (strstr(origin, "localhost") != nullptr ||
        strstr(origin, "127.0.0.1") != nullptr ||
        strstr(origin, "capacitor://") != nullptr ||
        strstr(origin, "http://192.168.") != nullptr ||
        strstr(origin, "https://192.168.") != nullptr ||
        strstr(origin, "http://10.") != nullptr ||
        strstr(origin, "https://10.") != nullptr ||
        strstr(origin, "http://172.") != nullptr ||
        strstr(origin, "https://172.") != nullptr) {
      allowOrigin = true;
    }
  } else {
    // 無 Origin Header（例如直接以瀏覽器網址列存取、ESP 序列監聽、cURL 或行動端非跨域連線）
    allowOrigin = true;
  }

  if (allowOrigin) {
    if (origin[0] != '\0') {
      httpd_resp_set_hdr(request, "Access-Control-Allow-Origin", origin);
      httpd_resp_set_hdr(request, "Vary", "Origin");
    } else {
      httpd_resp_set_hdr(request, "Access-Control-Allow-Origin", "*");
    }
  }
  // 若為非受信任外部公網 Origin（如惡意網站發動之跨網竊聽請求），則不予附帶 Access-Control-Allow-Origin 標頭，瀏覽器同源政策即時阻擋

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
    snprintf(errJson, sizeof(errJson),
             "{\"error\":\"CAMERA_NOT_READY\",\"code\":%d,\"detail\":\"%s\"}",
             static_cast<int>(cameraLastErrorCode), cameraLastError.c_str());
    Serial.printf("[CAPTURE ERROR] 拒絕拍攝請求: 相機尚未就緒 (代碼: 0x%x)\n", cameraLastErrorCode);
    return httpd_resp_send(request, errJson, HTTPD_RESP_USE_STRLEN);
  }

  camera_fb_t* frame = esp_camera_fb_get();
  if (!frame) {
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_status(request, "500 Internal Server Error");
    const char* errJson = "{\"error\":\"FRAME_CAPTURE_FAILED\",\"detail\":\"esp_camera_fb_get 逾時或記憶體配置失敗，無法獲取影格\"}";
    Serial.println("[CAPTURE ERROR] 取得影格失敗: esp_camera_fb_get 回傳 NULL");
    return httpd_resp_send(request, errJson, HTTPD_RESP_USE_STRLEN);
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
      const char* errJson = "{\"error\":\"JPEG_CONVERSION_FAILED\",\"detail\":\"RAW 像素轉成 JPEG 失敗\"}";
      Serial.println("[CAPTURE ERROR] JPEG 格式轉檔失敗");
      return httpd_resp_send(request, errJson, HTTPD_RESP_USE_STRLEN);
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
    char html[1536];
    snprintf(html, sizeof(html),
             "<!DOCTYPE html><html><head><meta charset='utf-8'><title>相機狀態診斷</title>"
             "<style>body{font-family:sans-serif;text-align:center;padding:40px;background:#18181b;color:#f4f4f5;}"
             ".card{background:#27272a;border-radius:12px;padding:24px;display:inline-block;max-width:540px;text-align:left;box-shadow:0 8px 30px rgba(0,0,0,0.6);line-height:1.6;}"
             "h2{color:#f59e0b;margin-top:0;}ul{padding-left:20px;}code{background:#3f3f46;padding:2px 6px;border-radius:4px;color:#38bdf8;}"
             ".btn{display:inline-block;margin-top:15px;padding:8px 16px;background:#3b82f6;color:#fff;text-decoration:none;border-radius:6px;font-weight:bold;}"
             "</style></head><body><div class='card'>"
             "<h2>⚠️ 相機模組尚未就緒</h2>"
             "<p><b>硬體錯誤代碼：</b><code>0x%x</code></p>"
             "<p><b>診斷說明：</b>%s</p>"
             "<hr style='border:1px solid #3f3f46;margin:15px 0;'>"
             "<p><b>建議排查步驟：</b></p>"
             "<ul><li>檢查 OV3660 鏡頭排線是否插緊到底、卡榫是否壓平扣緊</li><li>確認供電是否足夠（相機運作時瞬間峰值電流較高）</li><li>檢查腳位是否被其他感測器共用</li></ul>"
             "<a href='/' class='btn'>🔄 重新整理檢查</a> <a href='/status' class='btn' style='background:#6b7280;margin-left:10px;'>📊 系統遙測 JSON</a>"
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
      streamFailures++;
      if (streamFailures > 5) {
        Serial.println("[STREAM ERROR] 串流連續 5 次獲取影格失敗，終止連線");
        result = ESP_FAIL;
        break;
      }
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
      if (!converted) {
        Serial.println("[STREAM ERROR] 串流影格 JPEG 轉檔失敗");
        result = ESP_FAIL;
        break;
      }
    }

    size_t headerLength = snprintf(header, sizeof(header), STREAM_PART, jpegLength);
    result = httpd_resp_send_chunk(request, header, headerLength);
    if (result == ESP_OK) result = httpd_resp_send_chunk(request, reinterpret_cast<const char*>(jpegBuffer), jpegLength);
    if (result == ESP_OK) result = httpd_resp_send_chunk(request, "\r\n", 2);

    if (frame) esp_camera_fb_return(frame);
    if (converted && jpegBuffer) free(jpegBuffer);
    if (result != ESP_OK) {
      // 客戶端中斷連線（例如關閉瀏覽器分頁）為正常行為
      break;
    }
    delay(20);
  }
  return result;
}

static esp_err_t captureHandler(httpd_req_t* request) {
  addCorsHeaders(request);
  httpd_resp_set_type(request, "image/jpeg");
  return sendJpegFrame(request);
}

// ---------- 數位音訊前處理 (消除直流偏置 + 自適應增益 + VAD 語音活動檢測) ----------
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

  // 計算 RMS 能量（去除直流偏置後）
  double meanSquare = (double)(sumSquares / numSamples) - (double)dcOffset * dcOffset;
  if (meanSquare < 0) meanSquare = 0;
  double rms = sqrt(meanSquare);

  // VAD：RMS 能量低於門檻時判定為雜訊/靜音，不進行放大（避免送出無意義音訊給 Gemini）
  // 門檻設為 30：人聲即使偏小（RMS 30~80）仍放大 8 倍，確保 Gemini 轉寫得到足夠音量
  const double VAD_RMS_THRESHOLD = 30.0;
  if (rms < VAD_RMS_THRESHOLD) {
    Serial.printf("[AUDIO] 🔇 VAD 判定為雜訊/靜音 (RMS: %.1f < %.1f)，跳過放大，樣本數: %u\n",
                  rms, VAD_RMS_THRESHOLD, (unsigned int)numSamples);
    return;
  }

  // 自適應增益：根據 RMS 動態調整，最大 8 倍，避免過度放大導致失真
  int gain;
  if (rms > 3000) {
    gain = 2; // 已經夠大聲，只需微幅放大
  } else if (rms > 1000) {
    gain = 4;
  } else {
    gain = 8; // 較小的聲音才放大 8 倍
  }

  for (size_t i = 0; i < numSamples; i++) {
    int32_t val = (int32_t)(samples[i] - dcOffset) * gain;
    if (val > 32767) val = 32767;
    else if (val < -32768) val = -32768;
    samples[i] = (int16_t)val;
  }
  Serial.printf("[AUDIO] 🎙️ 音訊前處理完成: 消除 DC Offset (%d), 自適應增益 %d 倍, RMS: %.1f, 樣本數: %u\n",
                dcOffset, gain, rms, (unsigned int)numSamples);
}

static esp_err_t audioHandler(httpd_req_t* request) {
  addCorsHeaders(request);
  if (audioRecordedBytes == 0 || audioRecordBuffer == nullptr) {
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_status(request, "404 Not Found");
    const char* errJson = "{\"error\":\"NO_AUDIO_RECORDED\",\"detail\":\"尚無錄音資料，請先長按 BOOT 鍵錄音\"}";
    return httpd_resp_send(request, errJson, HTTPD_RESP_USE_STRLEN);
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
      size_t toSend = (audioRecordedBytes - sent > chunkSize) ? chunkSize : (audioRecordedBytes - sent);
      res = httpd_resp_send_chunk(request, reinterpret_cast<const char*>(audioRecordBuffer + sent), toSend);
      if (res != ESP_OK) break;
      sent += toSend;
    }
  }
  if (res == ESP_OK) {
    httpd_resp_send_chunk(request, nullptr, 0); // 傳輸結束標記
  }
  return res;
}

// ---------- 喇叭正弦波音調產生器 (免檔案、直接驅動 MAX98357) ----------
static void playTone(uint16_t freq, uint16_t durationMs) {
  if (!speakerReady) return;
  if (audioPlaybackBusy) return;
  audioPlaybackBusy = true;
  i2s_zero_dma_buffer(I2S_NUM_1);
  const uint32_t sampleRate = AUDIO_SAMPLE_RATE; // 16000
  const size_t totalSamples = (sampleRate * durationMs) / 1000;
  int16_t buffer[256 * 2]; // 立體聲雙聲道緩衝區 (LRCLK interleaved)
  float phase = 0.0f;
  const float phaseInc = 2.0f * 3.14159265f * freq / sampleRate;

  size_t samplesRemaining = totalSamples;
  while (samplesRemaining > 0) {
    size_t chunk = min((size_t)256, samplesRemaining);
    for (size_t i = 0; i < chunk; i++) {
      int16_t sample = (int16_t)(sinf(phase) * 16000.0f);
      phase += phaseInc;
      if (phase >= 2.0f * 3.14159265f) phase -= 2.0f * 3.14159265f;
      buffer[i * 2] = sample;     // 左聲道
      buffer[i * 2 + 1] = sample; // 右聲道
    }
    size_t bytesWritten = 0;
    i2s_write(I2S_NUM_1, buffer, chunk * sizeof(int16_t) * 2, &bytesWritten, portMAX_DELAY);
    samplesRemaining -= chunk;
  }
  audioPlaybackBusy = false;
}

// 開機成功旋律 (Do-Mi-Sol)
static void playStartupChime() {
  Serial.println("[AUDIO] 🔊 播放開機提示旋律...");
  playTone(523, 100); // C5
  delay(20);
  playTone(659, 100); // E5
  delay(20);
  playTone(784, 180); // G5
}

// 警報與按鍵提示音
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
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_status(request, "503 Service Unavailable");
    const char* err = "{\"error\":\"SPEAKER_NOT_READY\",\"detail\":\"MAX98357 擴大機尚未就緒\"}";
    return httpd_resp_send(request, err, HTTPD_RESP_USE_STRLEN);
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

  // 標記喇叭播放中，防止麥克風在此期間收音觸發回饋循環
  audioPlaybackActive = true;

  char buf[1024];
  int curLen = 0;
  uint8_t wavHeader[44] = {};
  size_t headerBytes = 0;
  bool headerParsed = false;
  audioPlaybackBusy = true;
  i2s_zero_dma_buffer(I2S_NUM_1);

  while (curLen < totalLen) {
    int received = httpd_req_recv(request, buf, sizeof(buf));
    if (received <= 0) {
      if (received == HTTPD_SOCK_ERR_TIMEOUT) continue;
      break;
    }
    curLen += received;

    uint8_t* playData = reinterpret_cast<uint8_t*>(buf);
    size_t playLen = static_cast<size_t>(received);

    // HTTP 接收不保證第一次就拿到完整 44-byte WAV header；先跨 chunk 累積，
    // 避免把標頭的一部分送進 I2S 造成喇叭無聲或爆音。
    if (!headerParsed) {
      const size_t needed = sizeof(wavHeader) - headerBytes;
      const size_t copyLen = min(needed, playLen);
      memcpy(wavHeader + headerBytes, playData, copyLen);
      headerBytes += copyLen;
      playData += copyLen;
      playLen -= copyLen;
      if (headerBytes < sizeof(wavHeader)) continue;

      const bool validWav = memcmp(wavHeader, "RIFF", 4) == 0 &&
                            memcmp(wavHeader + 8, "WAVE", 4) == 0;
      if (!validWav) {
        audioPlaybackBusy = false;
        httpd_resp_set_status(request, "415 Unsupported Media Type");
        return httpd_resp_send(request, "{\"error\":\"INVALID_WAV\"}", HTTPD_RESP_USE_STRLEN);
      }
      headerParsed = true;
    }

    if (playLen > 0) {
      // MAX98357 設定為 16-bit 雙聲道，維持 4-byte frame 對齊。
      playLen -= playLen % (sizeof(int16_t) * 2);
      if (playLen == 0) continue;
      size_t bytesWritten = 0;
      i2s_write(I2S_NUM_1, playData, playLen, &bytesWritten, portMAX_DELAY);
    }
  }

  audioPlaybackBusy = false;
  audioPlaybackActive = false;
  playbackEndTimeMs = millis(); // 記錄播放結束時間，啟動冷卻期

  httpd_resp_set_type(request, "application/json; charset=utf-8");
  return httpd_resp_send(request, "{\"status\":\"PLAY_COMPLETED\"}", HTTPD_RESP_USE_STRLEN);
}

// 支援瀏覽器 CORS Preflight (OPTIONS /play)
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

  char payload[512];
  snprintf(payload, sizeof(payload),
           "{\"triggerCounter\":%lu,\"audioCounter\":%lu,\"isRecording\":%s,"
           "\"micReady\":%s,\"speakerReady\":%s,\"audioBytes\":%lu,"
           "\"ip\":\"%s\",\"ultrasonicCm\":%.1f,\"emergencyBrake\":%s,"
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
  // 1. Port 80: 控制、語音收音 (/audio)、語音播放 (/play)、狀態與拍照端點
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

  esp_err_t serverErr = httpd_start(&cameraHttpd, &config);
  if (serverErr == ESP_OK) {
    httpd_register_uri_handler(cameraHttpd, &captureUri);
    httpd_register_uri_handler(cameraHttpd, &audioUri);
    httpd_register_uri_handler(cameraHttpd, &playUri);
    httpd_register_uri_handler(cameraHttpd, &playOptionsUri);
    httpd_register_uri_handler(cameraHttpd, &beepUri);
    httpd_register_uri_handler(cameraHttpd, &statusUri);
    Serial.println("[OK] Port 80 控制端點啟動成功: /capture, /audio, /play, /beep, /status");
  } else {
    Serial.printf("[ERROR] Port 80 HTTP 伺服器啟動失敗 (0x%x)\n", serverErr);
  }

  // 2. Port 81: 獨立影像串流專用伺服器 (避免阻塞語音與狀態端點)
  httpd_config_t streamConfig = HTTPD_DEFAULT_CONFIG();
  streamConfig.server_port = 81;
  streamConfig.ctrl_port = 32769;
  streamConfig.max_uri_handlers = 2;
  streamConfig.max_open_sockets = 4;
  streamConfig.lru_purge_enable = true;

  httpd_uri_t streamUri = { .uri = "/", .method = HTTP_GET, .handler = streamHandler, .user_ctx = nullptr };
  esp_err_t streamErr = httpd_start(&streamHttpd, &streamConfig);
  if (streamErr == ESP_OK) {
    httpd_register_uri_handler(streamHttpd, &streamUri);
    Serial.println("[OK] Port 81 MJPEG 影像串流伺服器啟動成功");
  } else {
    Serial.printf("[ERROR] Port 81 串流伺服器啟動失敗 (0x%x)\n", streamErr);
  }
}

static bool initCamera() {
  Serial.println("[INIT] 正在初始化相機模組 (DFRobot DFR1154 OV3660)...");
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
    Serial.println("--------------------------------------------------");
    Serial.printf("[CAMERA ERROR] 相機初始化失敗！代碼: 0x%x\n", cameraLastErrorCode);
    Serial.printf("原因分析: %s\n", cameraLastError.c_str());
    Serial.println("--------------------------------------------------");
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
      cameraModel = "Sensor PID: 0x" + String(sensor->id.PID, HEX);
    }
    Serial.printf("[OK] 成功識別相機感測器型號: %s\n", cameraModel.c_str());
  } else {
    Serial.println("[WARNING] esp_camera_sensor_get 回傳 NULL，使用預設影像設定");
    cameraModel = "Generic Camera";
  }

  // 丢棄開機前 2 張暖機影格，讓 OV3660 自動曝光與白平衡穩定
  for (int i = 0; i < 2; i++) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb) esp_camera_fb_return(fb);
    delay(40);
  }

  cameraReady = true;
  cameraLastError = "None";
  Serial.println("[OK] 相機影像驅動就緒 (VGA 640x480, 雙緩衝啟用)");
  return true;
}

// 解析 Wi-Fi 狀態碼中文訊息
static const char* getWiFiStatusDescription(wl_status_t status) {
  switch (status) {
    case WL_NO_SHIELD:       return "找不到 Wi-Fi 硬體模組";
    case WL_IDLE_STATUS:     return "正在等待 Wi-Fi 連線...";
    case WL_NO_SSID_AVAIL:   return "找不到指定之 Wi-Fi 名稱 (SSID)！請確認路由器是否開機或距離過遠。";
    case WL_SCAN_COMPLETED:  return "Wi-Fi 網路掃描完成";
    case WL_CONNECTED:       return "Wi-Fi 已成功建立連線！";
    case WL_CONNECT_FAILED:  return "Wi-Fi 連線失敗！密碼可能不正確，或路由器拒絕連線。";
    case WL_CONNECTION_LOST: return "Wi-Fi 連線訊號中斷";
    case WL_DISCONNECTED:    return "Wi-Fi 處於未連線狀態";
    default:                 return "未知的 Wi-Fi 狀態代碼";
  }
}

static void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("[NET] 正在連線至 Wi-Fi: '%s' (密碼: ******)\n", WIFI_SSID);

  const uint32_t deadline = millis() + 15000;
  while (WiFi.status() != WL_CONNECTED && millis() < deadline) {
    delay(500);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("==================================================");
    Serial.println("[OK] Wi-Fi 連線成功！");
    Serial.printf("  - 區域網路 IP : http://%s\n", WiFi.localIP().toString().c_str());
    Serial.printf("  - 子網路遮罩  : %s\n", WiFi.subnetMask().toString().c_str());
    Serial.printf("  - 預設閘道    : %s\n", WiFi.gatewayIP().toString().c_str());
    Serial.printf("  - Wi-Fi 訊號  : %d dBm\n", WiFi.RSSI());
    Serial.printf("  - 影像串流網址: http://%s/\n", WiFi.localIP().toString().c_str());
    Serial.printf("  - 單張拍照網址: http://%s/capture\n", WiFi.localIP().toString().c_str());
    Serial.printf("  - 系統遙測端點: http://%s/status\n", WiFi.localIP().toString().c_str());
    Serial.println("==================================================");
    return;
  }

  wl_status_t finalStatus = WiFi.status();
  Serial.println("--------------------------------------------------");
  Serial.printf("[WARNING] Wi-Fi STA 連線超時！狀態碼: %d\n", finalStatus);
  Serial.printf("診斷分析: %s\n", getWiFiStatusDescription(finalStatus));
  Serial.println("正在啟動備用無線熱點 (SoftAP) 模式...");
  Serial.println("--------------------------------------------------");

  WiFi.mode(WIFI_AP);
  bool apSuccess = WiFi.softAP(FALLBACK_AP_SSID, FALLBACK_AP_PASSWORD);
  if (apSuccess) {
    Serial.println("==================================================");
    Serial.println("[OK] 備用 Wi-Fi 熱點已建立！");
    Serial.printf("  - 熱點名稱 (SSID): %s\n", FALLBACK_AP_SSID);
    Serial.printf("  - 熱點密碼       : (已遮蔽)\n");
    Serial.printf("  - 相機網址       : http://%s/\n", WiFi.softAPIP().toString().c_str());
    Serial.println("  * 請使用手機 Wi-Fi 連接此熱點即可無線使用 *");
    Serial.println("==================================================");
  } else {
    Serial.println("[ERROR] 備用 Wi-Fi 熱點啟動失敗！請檢查射頻硬體。");
  }
}

static bool initAudio() {
  Serial.println("[INIT] 正在初始化音訊子系統 (DFRobot I2S PDM 麥克風 & MAX98357 擴大機)...");

  // 1. 分配錄音緩衝區 (約 192KB，優先使用 PSRAM)
  if (psramFound()) {
    audioRecordBuffer = (uint8_t*)ps_malloc(AUDIO_BUFFER_SIZE);
  } else {
    audioRecordBuffer = (uint8_t*)malloc(AUDIO_BUFFER_SIZE);
  }

  if (audioRecordBuffer) {
    Serial.printf("[OK] 成功分配音訊緩衝區: %u KB (%d 秒容量)\n", (unsigned int)(AUDIO_BUFFER_SIZE / 1024), MAX_AUDIO_SEC);
  } else {
    Serial.println("[CRITICAL ERROR] 無法分配音訊緩衝區！錄音功能將受限。");
  }

  // 2. 配置 I2S_NUM_0: PDM 數位麥克風 (RX)
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
    .ws_io_num = MIC_CLK_PIN, // 38
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = MIC_DATA_PIN // 39
  };
  esp_err_t micErr = i2s_driver_install(I2S_NUM_0, &mic_config, 0, NULL);
  if (micErr == ESP_OK) {
    i2s_set_pin(I2S_NUM_0, &mic_pins);
    micReady = true;
    Serial.println("[OK] 板載 I2S PDM 麥克風就緒 (16kHz, 16-bit Mono, CLK:38, DATA:39)");
  } else {
    Serial.printf("[AUDIO ERROR] 麥克風驅動安裝失敗 (0x%x)\n", micErr);
  }

  // 3. 配置 I2S_NUM_1: MAX98357 擴大機 (TX)
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
    .bck_io_num = SPK_BCLK_PIN, // 45
    .ws_io_num = SPK_LRCLK_PIN, // 46
    .data_out_num = SPK_DATA_PIN, // 42
    .data_in_num = I2S_PIN_NO_CHANGE
  };
  esp_err_t spkErr = i2s_driver_install(I2S_NUM_1, &spk_config, 0, NULL);
  if (spkErr == ESP_OK) {
    i2s_set_pin(I2S_NUM_1, &spk_pins);
    speakerReady = true;
    Serial.println("[OK] 板載 MAX98357 擴大機喇叭就緒 (BCLK:45, LRCLK:46, DATA:42)");
  } else {
    Serial.printf("[AUDIO ERROR] 喇叭擴大機驅動安裝失敗 (0x%x)\n", spkErr);
  }

  return (micReady || speakerReady);
}

static float readUltrasonicDistanceCm() {
  digitalWrite(ULTRASONIC_TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(ULTRASONIC_TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(ULTRASONIC_TRIG_PIN, LOW);

  const unsigned long duration = pulseIn(ULTRASONIC_ECHO_PIN, HIGH, ULTRASONIC_TIMEOUT_US);
  if (duration == 0) return 400.0f;
  const float dist = duration * 0.0343f / 2.0f;
  return (dist > 1.0f && dist < 450.0f) ? dist : 400.0f;
}

static void updateLayer0EmergencyBrake() {
  const uint32_t now = millis();
  // Sample every ~25ms (< 30ms latency for urgent brake)
  if (now - lastSonarSampleMs >= 25) {
    lastSonarSampleMs = now;
    const float rawDist = readUltrasonicDistanceCm();
    if (rawDist < chestDistanceCm) {
      chestDistanceCm = rawDist;
    } else {
      chestDistanceCm = 0.7f * chestDistanceCm + 0.3f * rawDist;
    }
    emergencyBrakeActive = (chestDistanceCm < EMERGENCY_BRAKE_DIST_CM);
  }

  // Active fast pulse alarm when in emergency brake distance
  if (emergencyBrakeActive) {
    if (now - lastAlarmToggleMs >= 35) { // Rapid 35ms pulse
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

  Serial.println("\n");
  Serial.println("╔════════════════════════════════════════════════════════╗");
  Serial.println("║   Blind Assist - Chest Vision Node (DFRobot DFR1154)   ║");
  Serial.println("╚════════════════════════════════════════════════════════╝");

  // 1. 硬體記憶體與 PSRAM 自我檢測
  if (psramFound()) {
    Serial.printf("[OK] PSRAM 已成功啟用！總容量: %u KB (可用: %u KB)\n",
                  ESP.getPsramSize() / 1024, ESP.getFreePsram() / 1024);
  } else {
    Serial.println("[CRITICAL WARNING] 未偵測到 PSRAM！相機雙緩衝可能無法配置，請確認 platformio.ini 是否開啟 OPI PSRAM。");
  }
  Serial.printf("[INFO] 內部 Heap 剩餘記憶體: %u KB\n", ESP.getFreeHeap() / 1024);

  // 2. 引腳初始化 (加強 GPIO 0 內部上拉，避免腳位浮動誤觸)
  pinMode(TRIGGER_BUTTON_PIN, INPUT_PULLUP);
  gpio_pullup_en(GPIO_NUM_0);
  gpio_pulldown_dis(GPIO_NUM_0);

  pinMode(ULTRASONIC_TRIG_PIN, OUTPUT);
  pinMode(ULTRASONIC_ECHO_PIN, INPUT);
  pinMode(BRAKE_ALARM_PIN, OUTPUT);
  digitalWrite(ULTRASONIC_TRIG_PIN, LOW);
  digitalWrite(BRAKE_ALARM_PIN, LOW);
  Serial.println("[OK] GPIO 引腳配置完成 (Trigger: 0, Trig: 43, Echo: 44, Alarm LED: 3)");

  // 3. 相機模組初始化
  if (!initCamera()) {
    Serial.println("==================================================");
    Serial.println("[NOTICE] 相機雖未就緒，系統仍將繼續連線 Wi-Fi 並啟動網路端點。");
    Serial.println("連線後可開啟瀏覽器存取 /status 或 / 檢視診斷儀表板。");
    Serial.println("==================================================");
  }

  // 4. 音訊子系統初始化 (PDM 麥克風 & 喇叭)
  initAudio();

  // 5. 網路連線與伺服器啟動
  connectWiFi();
  startCameraServer();

  // 6. 開機成功提示旋律 (Do-Mi-Sol)
  playStartupChime();

  Serial.println("[OK] 第 0 層硬體急煞守護已就緒 (< 80cm 自動警報，取樣週期 25ms)。");
  Serial.println("[OK] 對胸前提問語音互動已就緒 (長按按鈕 > 350ms 錄音提問，放開自動拍照問答)。");
}

void loop() {
  updateLayer0EmergencyBrake();

  const uint32_t now = millis();

  // 按鍵消抖處理：避免電氣噪聲或彈跳造成誤觸發
  const bool rawPressed = (digitalRead(TRIGGER_BUTTON_PIN) == LOW);
  if (rawPressed != lastRawButtonState) {
    lastDebounceMs = now;
  }
  bool pressed = false;
  if (now - lastDebounceMs > DEBOUNCE_MS) {
    pressed = rawPressed;
  }
  lastRawButtonState = rawPressed;

  // 診斷：每 2 秒輸出一次按鈕狀態（方便確認 GPIO 0 是否正常）
  static uint32_t lastDebugMs = 0;
  if (now - lastDebugMs > 2000) {
    lastDebugMs = now;
    Serial.printf("[DEBUG] GPIO0=%d, rawPressed=%d, pressed=%d, buttonHeld=%d, buttonLockout=%d, micReady=%s, audioPlaybackActive=%s\n",
                  digitalRead(TRIGGER_BUTTON_PIN),
                  rawPressed ? 1 : 0,
                  pressed ? 1 : 0,
                  buttonHeld ? 1 : 0,
                  buttonLockout ? 1 : 0,
                  micReady ? "true" : "false",
                  audioPlaybackActive ? "true" : "false");
  }

  // 按鍵放開時解除鎖定
  if (!pressed && buttonLockout) {
    buttonLockout = false;
    Serial.println("[BUTTON] 🔓 按鈕已放開，解除鎖定");
  }

  // 按鍵按下處理
  if (pressed) {
    // 鎖定狀態下忽略所有觸發（防止 7 秒安全機制無限循環）
    if (buttonLockout) {
      // 不做任何事，等待按鈕放開
    } else if (!buttonHeld) {
      buttonHeld = true;
      buttonPressStartMs = now;
      Serial.printf("[BUTTON] 🔘 按鈕按下 (holdTime=0, micReady=%s, audioPlaybackActive=%s, cooldown=%ums)\n",
                    micReady ? "true" : "false",
                    audioPlaybackActive ? "true" : "false",
                    (unsigned int)(now - playbackEndTimeMs));
    } else {
      const uint32_t holdTime = now - buttonPressStartMs;

      // 按住超過 350ms: 進入對話提問錄音模式
      if (!isRecording && holdTime > 350) {
        Serial.printf("[BUTTON] ⏱️ 按住超過 350ms (holdTime=%ums, micReady=%s, audioPlaybackActive=%s, cooldownRemaining=%ums)\n",
                      (unsigned int)holdTime,
                      micReady ? "true" : "false",
                      audioPlaybackActive ? "true" : "false",
                      (millis() - playbackEndTimeMs < PLAYBACK_COOLDOWN_MS) ? (unsigned int)(PLAYBACK_COOLDOWN_MS - (millis() - playbackEndTimeMs)) : 0);
        // 防止回饋循環：喇叭播放中或冷卻期內不啟動錄音
        if (audioPlaybackActive) {
          Serial.println("[AUDIO] ⏸️ 喇叭播放中，忽略錄音觸發（防止回饋循環）");
        } else if (playbackEndTimeMs != 0 && millis() - playbackEndTimeMs < PLAYBACK_COOLDOWN_MS) {
          Serial.printf("[AUDIO] ⏸️ 喇叭冷卻期內 (< %u ms)，忽略錄音觸發\n", PLAYBACK_COOLDOWN_MS);
        } else {
          isRecording = true;
          audioRecordedBytes = 0;
          // 丟棄按鍵前殘留的 DMA 資料，確保 /audio 只包含本次提問。
          if (micReady) {
            i2s_zero_dma_buffer(I2S_NUM_0);
            i2s_start(I2S_NUM_0);
          }
          digitalWrite(BRAKE_ALARM_PIN, HIGH); // 錄音指示燈恆亮
          playTone(800, 70); // 提問收音開始提示音
          Serial.println("[AUDIO] 🎙️ 進入按住說話錄音提問模式...");
        }
      }

      // 正在錄音中：持續從 I2S PDM 麥克風讀取音訊
      if (isRecording && audioRecordBuffer && micReady) {
        // 若喇叭開始播放（AI 回答），立即停止錄音避免收到喇叭聲音
        if (audioPlaybackActive) {
          isRecording = false;
          digitalWrite(BRAKE_ALARM_PIN, LOW);
          if (micReady) i2s_stop(I2S_NUM_0);
          Serial.println("[AUDIO] ⏸️ 喇叭播放中，自動停止錄音");
        } else if (audioRecordedBytes < AUDIO_BUFFER_SIZE) {
          size_t bytesRead = 0;
          size_t bytesToRead = min((size_t)1024, (size_t)(AUDIO_BUFFER_SIZE - audioRecordedBytes));
          esp_err_t r = i2s_read(I2S_NUM_0, audioRecordBuffer + audioRecordedBytes, bytesToRead, &bytesRead, pdMS_TO_TICKS(10));
          if (r == ESP_OK && bytesRead > 0) {
            audioRecordedBytes += bytesRead;
          }
        } else {
          // 達到最大長度上限 (6秒)，自動結束錄音與拍照
          isRecording = false;
          digitalWrite(BRAKE_ALARM_PIN, LOW);
          if (micReady) i2s_stop(I2S_NUM_0);
          processAndBoostAudio(audioRecordBuffer, audioRecordedBytes);
          playTone(1500, 80);
          audioCounter++;
          triggerCounter++;
          Serial.printf("[AUDIO] 🛑 錄音已達 6 秒上限，自動結束！bytes: %lu, audioCounter=%lu\n",
                        static_cast<unsigned long>(audioRecordedBytes),
                        static_cast<unsigned long>(audioCounter));
        }
      }

      // 安全機制：若按鍵長按超過 7 秒（可能為腳位懸空或開機誤觸），鎖定按鈕直到放開
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
        buttonLockout = true; // 鎖定按鈕，直到真正放開為止
        Serial.println("[BUTTON] 🔒 按鍵長按超過 7 秒，鎖定按鈕直到放開（防止無限循環嗶叫）");
      }
    }
  } else {
    // 按鍵放開處理
    if (buttonHeld) {
      const uint32_t pressDuration = now - buttonPressStartMs;
      if (isRecording) {
        // 錄音提問結束：放開按鈕自動進行音訊前處理 (去偏置+放大)、拍照並送出問答
        isRecording = false;
        digitalWrite(BRAKE_ALARM_PIN, LOW);
        if (micReady) i2s_stop(I2S_NUM_0); // 停止 I2S 麥克風，節省資源
        processAndBoostAudio(audioRecordBuffer, audioRecordedBytes);
        playTone(1500, 80); // 提問收音結束提示音
        audioCounter++;
        triggerCounter++; // 同時遞增 triggerCounter 進行拍照
        Serial.printf("[AUDIO] 🛑 錄音提問結束！收音長度: %lu bytes, audioCounter=%lu, 同步觸發拍照問答。\n",
                      static_cast<unsigned long>(audioRecordedBytes),
                      static_cast<unsigned long>(audioCounter));
      } else if (pressDuration > 50 && (now - lastButtonChangeMs > 700)) {
        // 短按 (< 350ms): 純手動拍照快門
        lastButtonChangeMs = now;
        triggerCounter++;
        playTone(1200, 50); // 快門提示音
        Serial.printf("[TRIGGER] 📸 實體按鍵短按觸發拍照: %lu; 手機將抓取 /capture。\n",
                      static_cast<unsigned long>(triggerCounter));
      }
      buttonHeld = false;
    }
  }

  delay(5);
}
