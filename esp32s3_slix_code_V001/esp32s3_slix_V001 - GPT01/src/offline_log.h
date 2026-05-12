// Offline Telemetry Queue
//
// Automatically selects storage backend:
//   SD card  — when SD is mounted (WiFi mode)
//   LittleFS — fallback (LAN mode or no SD card)
//
// Queue layout (both backends use the same file structure):
//   /offline/head     — index of oldest unack'd batch
//   /offline/tail     — next index to write
//   /offline/{n}.json — one telemetry batch per file
//
// Capacity:
//   SD card  : effectively unlimited (32 GB)
//   LittleFS : ~9.9 MB → ~3300 batches ≈ 55 hours at 60s interval
#pragma once
#include <Arduino.h>
#include <LittleFS.h>
#include "sd_card.h"

#define OFFLINE_DIR       "/offline"
#define OFFLINE_HEAD_FILE "/offline/head"
#define OFFLINE_TAIL_FILE "/offline/tail"
#define OFFLINE_MAX_LFS   3300   // LittleFS limit (~55 hr)
#define OFFLINE_MAX_SD    500000 // SD limit (effectively unlimited)

static uint32_t _offHead = 0;
static uint32_t _offTail = 0;
static bool     _offUseSD = false;

// ── Transparent file I/O (SD or LittleFS) ────────────────────────────────
static bool _ofExists(const char* p) {
  return _offUseSD ? SD.exists(p) : LittleFS.exists(p);
}
static void _ofRemove(const char* p) {
  if (_offUseSD) SD.remove(p); else LittleFS.remove(p);
}
static bool _ofWrite(const char* p, const char* data) {
  if (_offUseSD) {
    File f = SD.open(p, FILE_WRITE);
    if (!f) return false;
    f.print(data); f.close(); return true;
  } else {
    File f = LittleFS.open(p, "w");
    if (!f) return false;
    f.print(data); f.close(); return true;
  }
}
static bool _ofRead(const char* p, char* buf, size_t maxLen) {
  File f = _offUseSD ? SD.open(p, FILE_READ) : LittleFS.open(p, "r");
  if (!f) return false;
  size_t n = f.readBytes(buf, maxLen - 1);
  buf[n] = '\0';
  f.close();
  return n > 0;
}

static uint32_t _offReadIdx(const char* path) {
  char buf[16] = {};
  _ofRead(path, buf, sizeof(buf));
  return (uint32_t)atol(buf);
}
static void _offWriteIdx(const char* path, uint32_t v) {
  char buf[16]; snprintf(buf, sizeof(buf), "%u", v);
  _ofWrite(path, buf);
}
static void _offBatchPath(char* out, size_t sz, uint32_t idx) {
  snprintf(out, sz, "/offline/%u.json", idx);
}

// ── Public API ────────────────────────────────────────────────────────────

// Call after sdInit() and LittleFS mount. Pass sdAvailable() result.
inline void offlineInit(bool useSD = false) {
  _offUseSD = useSD;
  const char* backend = _offUseSD ? "SD card" : "LittleFS";

  // Ensure directory exists
  if (_offUseSD) {
    if (!SD.exists(OFFLINE_DIR)) SD.mkdir(OFFLINE_DIR);
  } else {
    if (!LittleFS.exists(OFFLINE_DIR)) LittleFS.mkdir(OFFLINE_DIR);
  }

  _offHead = _offReadIdx(OFFLINE_HEAD_FILE);
  _offTail = _offReadIdx(OFFLINE_TAIL_FILE);

  uint32_t maxF = _offUseSD ? OFFLINE_MAX_SD : OFFLINE_MAX_LFS;
  uint32_t pending = (_offTail >= _offHead) ? (_offTail - _offHead)
                                             : (maxF - _offHead + _offTail);

  Serial.printf("[OFFLINE] Backend: %s | Pending: %u batches (head=%u tail=%u)\n",
                backend, pending, _offHead, _offTail);
}

inline bool offlineHasPending() { return _offHead != _offTail; }

inline uint32_t offlinePendingCount() {
  uint32_t maxF = _offUseSD ? OFFLINE_MAX_SD : OFFLINE_MAX_LFS;
  return (_offTail >= _offHead) ? (_offTail - _offHead)
                                : (maxF - _offHead + _offTail);
}

// Enqueue one telemetry batch (JSON string)
inline bool offlineEnqueue(const char* json) {
  uint32_t maxF = _offUseSD ? OFFLINE_MAX_SD : OFFLINE_MAX_LFS;
  uint32_t next = (_offTail + 1) % maxF;

  if (next == _offHead) {
    // Full: drop oldest to make room
    char path[48]; _offBatchPath(path, sizeof(path), _offHead);
    _ofRemove(path);
    _offHead = (_offHead + 1) % maxF;
    _offWriteIdx(OFFLINE_HEAD_FILE, _offHead);
    Serial.println("[OFFLINE] Queue full — dropped oldest batch");
  }

  char path[48]; _offBatchPath(path, sizeof(path), _offTail);
  if (!_ofWrite(path, json)) {
    Serial.println("[OFFLINE] Write error");
    return false;
  }

  _offTail = next;
  _offWriteIdx(OFFLINE_TAIL_FILE, _offTail);

  uint32_t idx = (_offTail == 0 ? maxF - 1 : _offTail - 1);
  Serial.printf("[OFFLINE] Queued batch #%u (%s, pending=%u)\n",
                idx, _offUseSD ? "SD" : "LFS", offlinePendingCount());
  return true;
}

// Read oldest batch into buf. Returns true on success.
inline bool offlinePeek(char* buf, size_t maxLen) {
  if (!offlineHasPending()) return false;
  char path[48]; _offBatchPath(path, sizeof(path), _offHead);
  if (!_ofRead(path, buf, maxLen)) {
    // Corrupt entry — skip
    Serial.printf("[OFFLINE] Corrupt batch #%u — skipping\n", _offHead);
    uint32_t maxF = _offUseSD ? OFFLINE_MAX_SD : OFFLINE_MAX_LFS;
    _offHead = (_offHead + 1) % maxF;
    _offWriteIdx(OFFLINE_HEAD_FILE, _offHead);
    return false;
  }
  return true;
}

// Acknowledge (delete) oldest batch after successful send
inline void offlineAck() {
  if (!offlineHasPending()) return;
  char path[48]; _offBatchPath(path, sizeof(path), _offHead);
  _ofRemove(path);
  uint32_t maxF = _offUseSD ? OFFLINE_MAX_SD : OFFLINE_MAX_LFS;
  _offHead = (_offHead + 1) % maxF;
  _offWriteIdx(OFFLINE_HEAD_FILE, _offHead);
}
