// SD Card Support
//
// SD card pins on Waveshare ESP32-S3-ETH:
//   CLK=GPIO7  MOSI=GPIO6  MISO=GPIO5  CS=GPIO4
//
// SD uses HSPI (SPI3) so it can run independently from W5500 (FSPI/SPI2).
// → SD card available in BOTH LAN and WiFi modes.
//
// This header provides:
//   sdInit()       — call once in setup(); returns true if SD mounted
//   sdAvailable()  — true if SD is mounted and usable
#pragma once
#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include "eth_config.h"

#define SD_SCK_PIN  7
#define SD_MISO_PIN 5
#define SD_MOSI_PIN 6
#define SD_CS_PIN   4

// HSPI (SPI3) — independent from W5500 which uses FSPI (SPI2)
static SPIClass _sdSpi(HSPI);
static bool     _sdReady = false;

// Call once in setup(). Returns true if SD card successfully mounted.
inline bool sdInit(uint8_t /*mode unused, kept for API compat*/ = 0) {
  _sdSpi.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
  if (!SD.begin(SD_CS_PIN, _sdSpi)) {
    Serial.println("[SD] Mount FAILED — no card or format error");
    Serial.println("[SD] Offline storage will fall back to LittleFS");
    _sdReady = false;
    return false;
  }

  uint64_t cardMB = SD.cardSize() / (1024ULL * 1024ULL);
  Serial.printf("[SD] Mounted OK — %.1f GB  type=%d\n",
                cardMB / 1024.0f, (int)SD.cardType());

  if (!SD.exists("/offline")) SD.mkdir("/offline");
  if (!SD.exists("/log"))     SD.mkdir("/log");

  _sdReady = true;
  return true;
}

inline bool sdAvailable() { return _sdReady; }

// ── SD helpers (mirror LittleFS API) ─────────────────────────────────────
inline bool sdFileExists(const char* path)           { return SD.exists(path); }
inline void sdRemoveFile(const char* path)           { SD.remove(path); }
inline bool sdMkdir(const char* path)                { return SD.mkdir(path); }

inline bool sdWriteFile(const char* path, const char* data) {
  File f = SD.open(path, FILE_WRITE);
  if (!f) return false;
  f.print(data);
  f.close();
  return true;
}

inline bool sdReadFile(const char* path, char* buf, size_t maxLen) {
  File f = SD.open(path, FILE_READ);
  if (!f) return false;
  size_t len = f.readBytes(buf, maxLen - 1);
  buf[len] = '\0';
  f.close();
  return len > 0;
}

// Append a line to a log file on SD (creates if not exists)
inline bool sdAppendLog(const char* path, const char* line) {
  File f = SD.open(path, FILE_APPEND);
  if (!f) return false;
  f.println(line);
  f.close();
  return true;
}
