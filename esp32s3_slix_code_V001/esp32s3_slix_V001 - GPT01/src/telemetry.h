// Telemetry Buffer, Aggregation, and ThingsBoard Payload Builder
// Each active sensor slot has a FIFO ring buffer of SensorData readings.
// Every 60s: aggregate (median / circular-mean for wind) → build JSON payload.
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <RTClib.h>
#include <math.h>
#include "sensor_types.h"
#include "sensor_registry.h"

extern RTC_DS3231 rtc;

#define MAX_BUF_DEPTH  20  // max buffer depth (rainfall uses 20)

// ── Per-slot FIFO buffer ──────────────────────────────────────────────────
struct TelFrame {
  bool    valid;
  float   values[MAX_SENSOR_FIELDS];
  uint8_t field_count;
  uint32_t ts_unix; // unix seconds at time of reading
};

static TelFrame  _buf[MAX_SENSOR_SLOTS][MAX_BUF_DEPTH] = {};
static uint8_t   _head[MAX_SENSOR_SLOTS]  = {};
static uint8_t   _count[MAX_SENSOR_SLOTS] = {};
static float     _rainAcc[MAX_SENSOR_SLOTS] = {}; // rainfall accumulator per slot
static SemaphoreHandle_t _telMutex = nullptr;

// ── Math helpers ──────────────────────────────────────────────────────────
static float _median(float* v, int n) {
  // Insertion sort (n ≤ 20, fast enough)
  for (int i = 1; i < n; i++) {
    float key = v[i];
    int j = i - 1;
    while (j >= 0 && v[j] > key) { v[j+1] = v[j]; j--; }
    v[j+1] = key;
  }
  return (n & 1) ? v[n/2] : (v[n/2-1] + v[n/2]) * 0.5f;
}

static float _circularMean(float* angles, int n) {
  float sinSum = 0.0f, cosSum = 0.0f;
  for (int i = 0; i < n; i++) {
    float r = angles[i] * DEG_TO_RAD;
    sinSum += sinf(r);
    cosSum += cosf(r);
  }
  float a = atan2f(sinSum, cosSum) * RAD_TO_DEG;
  return fmodf(a + 360.0f, 360.0f);
}

// ── Public API ────────────────────────────────────────────────────────────
inline void telemetryInit() {
  _telMutex = xSemaphoreCreateMutex();
  memset(_buf,     0, sizeof(_buf));
  memset(_head,    0, sizeof(_head));
  memset(_count,   0, sizeof(_count));
  memset(_rainAcc, 0, sizeof(_rainAcc));
}

// Append a sensor reading to the slot's FIFO buffer
inline void telemetryAppend(int slot, SensorTypeID type, const SensorData& data) {
  if (slot < 0 || slot >= MAX_SENSOR_SLOTS) return;
  if (!data.valid || !_telMutex) return;

  if (xSemaphoreTake(_telMutex, pdMS_TO_TICKS(200)) != pdTRUE) return;

  uint8_t buf_size = SENSOR_TYPES[type].buf_size;
  if (buf_size > MAX_BUF_DEPTH) buf_size = MAX_BUF_DEPTH;

  // Get RTC timestamp
  DateTime now = rtc.now();

  uint8_t pos;
  if (_count[slot] >= buf_size) {
    // Overwrite oldest
    pos = _head[slot];
    _head[slot] = (_head[slot] + 1) % buf_size;
  } else {
    pos = (_head[slot] + _count[slot]) % buf_size;
    _count[slot]++;
  }

  TelFrame& f  = _buf[slot][pos];
  f.valid       = true;
  f.ts_unix     = now.unixtime();
  f.field_count = data.field_count;
  for (uint8_t i = 0; i < data.field_count && i < MAX_SENSOR_FIELDS; i++)
    f.values[i] = data.fields[i].val;

  // Rainfall accumulator
  if (type == ST_RAINFALL)
    _rainAcc[slot] += data.fields[0].val;

  xSemaphoreGive(_telMutex);
}

// Build full ThingsBoard gateway telemetry JSON for all active sensors.
// Clears buffers after building. Caller must free nothing (static buf returned).
// ts_last_valid[slot] is populated with the unix timestamp of the last reading.
static char _payloadBuf[8192];

inline const char* telemetryBuildPayload() {
  if (!_telMutex) return nullptr;
  if (xSemaphoreTake(_telMutex, pdMS_TO_TICKS(1000)) != pdTRUE) return nullptr;

  DynamicJsonDocument doc(8192);

  SensorEntry* entries[MAX_SENSOR_SLOTS];
  int cnt = registryGetAll(entries, MAX_SENSOR_SLOTS);

  for (int i = 0; i < cnt; i++) {
    SensorEntry* e = entries[i];
    int sl = e->slot;
    SensorTypeID type = e->type_id;
    uint8_t buf_size  = SENSOR_TYPES[type].buf_size;
    if (buf_size > MAX_BUF_DEPTH) buf_size = MAX_BUF_DEPTH;

    // Count valid frames
    int valid = 0;
    uint32_t last_ts = 0;
    for (uint8_t b = 0; b < _count[sl]; b++) {
      TelFrame& fr = _buf[sl][(_head[sl] + b) % buf_size];
      if (fr.valid) { valid++; last_ts = fr.ts_unix; }
    }

    bool connected = (_count[sl] > 0);
    bool healthy   = (valid > 0);
    uint64_t ts_ms = ((uint64_t)(last_ts > 0 ? last_ts : rtc.now().unixtime())) * 1000ULL;

    JsonArray arr = doc.createNestedArray(e->device_key);
    JsonObject msg = arr.createNestedObject();
    msg["ts"] = ts_ms;
    JsonObject vals = msg.createNestedObject("values");
    vals["current_status"]   = healthy   ? "healthy" : "weekly";
    vals["operation_status"] = connected ? "online"  : "offline";

    if (healthy) {
      if (type == ST_RAINFALL) {
        vals["rainfall"] = roundf(_rainAcc[sl] * 100.0f) / 100.0f;
        _rainAcc[sl] = 0.0f;
      } else {
        uint8_t fc = _buf[sl][_head[sl]].field_count;
        for (uint8_t f = 0; f < fc; f++) {
          float pool[MAX_BUF_DEPTH];
          int pn = 0;
          for (uint8_t b = 0; b < _count[sl]; b++) {
            TelFrame& fr = _buf[sl][(_head[sl] + b) % buf_size];
            if (fr.valid && f < fr.field_count) pool[pn++] = fr.values[f];
          }
          if (pn == 0) continue;

          float agg;
          bool isWindDir = (type == ST_WIND && f == 1);
          if (isWindDir) agg = _circularMean(pool, pn);
          else           agg = _median(pool, pn);

          vals[sensorFieldKey(type, f)] = roundf(agg * 100.0f) / 100.0f;
        }
      }
    }

    // Clear buffer
    _count[sl] = 0;
    _head[sl]  = 0;
  }

  xSemaphoreGive(_telMutex);

  if (doc.size() == 0) return nullptr;
  serializeJson(doc, _payloadBuf, sizeof(_payloadBuf));
  return _payloadBuf;
}

// Build payload for a single slot using a specific past timestamp
// (used for offline replay where ts_unix is stored per batch)
inline const char* telemetryBuildSingleSlot(int slot, uint32_t ts_unix) {
  if (!_telMutex) return nullptr;
  SensorEntry* e = registryGetBySlot(slot);
  if (!e) return nullptr;

  SensorTypeID type = e->type_id;
  uint8_t buf_size  = SENSOR_TYPES[type].buf_size;
  if (buf_size > MAX_BUF_DEPTH) buf_size = MAX_BUF_DEPTH;

  if (xSemaphoreTake(_telMutex, pdMS_TO_TICKS(500)) != pdTRUE) return nullptr;

  DynamicJsonDocument doc(2048);
  JsonArray arr = doc.createNestedArray(e->device_key);
  JsonObject msg = arr.createNestedObject();
  msg["ts"] = (uint64_t)ts_unix * 1000ULL;
  JsonObject vals = msg.createNestedObject("values");
  vals["current_status"]   = "healthy";
  vals["operation_status"] = "online";

  if (type == ST_RAINFALL) {
    vals["rainfall"] = roundf(_rainAcc[slot] * 100.0f) / 100.0f;
  } else {
    uint8_t fc = (_count[slot] > 0) ? _buf[slot][_head[slot]].field_count : 0;
    for (uint8_t f = 0; f < fc; f++) {
      float pool[MAX_BUF_DEPTH]; int pn = 0;
      for (uint8_t b = 0; b < _count[slot]; b++) {
        TelFrame& fr = _buf[slot][(_head[slot] + b) % buf_size];
        if (fr.valid && f < fr.field_count) pool[pn++] = fr.values[f];
      }
      if (pn == 0) continue;
      bool isWindDir = (type == ST_WIND && f == 1);
      float agg = isWindDir ? _circularMean(pool, pn) : _median(pool, pn);
      vals[sensorFieldKey(type, f)] = roundf(agg * 100.0f) / 100.0f;
    }
  }

  xSemaphoreGive(_telMutex);
  serializeJson(doc, _payloadBuf, sizeof(_payloadBuf));
  return _payloadBuf;
}
