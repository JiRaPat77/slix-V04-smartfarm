// Simple application log helper
// - มิเรอร์ Serial → ไฟล์ log บน LittleFS (หรือ SD ถ้ามี)
// - ใช้แทน Serial.printf ในจุดสำคัญ เพื่อให้เก็บประวัติได้
// - ไฟล์: /log/app.log (rotate เมื่อเกิน LOG_MAX_BYTES)
//
// API:
//   logInit(useSD=false)        — เรียกหลัง LittleFS/SD mount
//   LOGF("[TAG] fmt", args...)  — printf-style, มิเรอร์ Serial + file
#pragma once
#include <Arduino.h>
#include <LittleFS.h>
#include <RTClib.h>

extern RTC_DS3231 rtc;

#define LOG_PATH        "/log/app.log"
#define LOG_PATH_OLD    "/log/app.old"
#define LOG_DIR         "/log"
#define LOG_MAX_BYTES   524288   // 512 KB → rotate

static bool _logUseSD    = false;
static bool _logReady    = false;
static SemaphoreHandle_t _logMutex = nullptr;

inline void logInit(bool useSD = false) {
  _logUseSD = useSD;
  _logMutex = xSemaphoreCreateMutex();

  if (_logUseSD) {
    // SD จะถูก mount จาก sd_card.h ก่อนหน้านี้
    _logReady = true;
  } else {
    if (!LittleFS.begin(true)) {
      Serial.println("[LOG] LittleFS mount FAILED");
      _logReady = false;
      return;
    }
    if (!LittleFS.exists(LOG_DIR)) LittleFS.mkdir(LOG_DIR);
    _logReady = true;
  }
  Serial.printf("[LOG] Ready (backend=%s)\n", _logUseSD ? "SD" : "LittleFS");
}

inline bool logAvailable() { return _logReady; }

static void _logRotateIfNeeded() {
  if (!_logReady) return;
  size_t sz = 0;
  if (_logUseSD) {
    // SD path — skipped here (would need #include "sd_card.h")
  } else {
    File f = LittleFS.open(LOG_PATH, "r");
    if (f) { sz = f.size(); f.close(); }
    if (sz >= LOG_MAX_BYTES) {
      LittleFS.remove(LOG_PATH_OLD);
      LittleFS.rename(LOG_PATH, LOG_PATH_OLD);
    }
  }
}

inline void logWrite(const char* line) {
  if (!_logReady || !_logMutex) return;
  if (xSemaphoreTake(_logMutex, pdMS_TO_TICKS(200)) != pdTRUE) return;

  _logRotateIfNeeded();
  if (!_logUseSD) {
    File f = LittleFS.open(LOG_PATH, "a");
    if (f) {
      // timestamp prefix
      DateTime t = rtc.now();
      char ts[24];
      snprintf(ts, sizeof(ts), "%04d-%02d-%02d %02d:%02d:%02d ",
               t.year(), t.month(), t.day(), t.hour(), t.minute(), t.second());
      f.print(ts);
      f.println(line);
      f.close();
    }
  }

  xSemaphoreGive(_logMutex);
}

#define LOGF(fmt, ...) do {                                  \
  char _lbuf[256];                                           \
  snprintf(_lbuf, sizeof(_lbuf), fmt, ##__VA_ARGS__);        \
  Serial.println(_lbuf);                                     \
  Serial.flush();                                            \
  logWrite(_lbuf);                                           \
} while (0)
