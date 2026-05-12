// Sensor Registry — active and reserved sensors persisted in LittleFS
// active_sensors.json  : sensors currently connected (local RS485 or LoRa virtual)
// reserved_sensors.json: recently disconnected sensors (kept 30 days)
#pragma once
#include <Arduino.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "sensor_types.h"

#define MAX_SENSOR_SLOTS   16
#define RESERVED_MAX_SECS  (30UL * 24 * 3600)  // 30 days
#define DEVICE_PREFIX      "SLXA1260004"
#define ACTIVE_PATH        "/sensors/active.json"
#define RESERVED_PATH      "/sensors/reserved.json"

// ── Sensor Entry (active) ─────────────────────────────────────────────────
struct SensorEntry {
  bool         occupied;
  uint8_t      slot;           // index in _active[] (0-15)
  uint8_t      port;           // 1-16 for local RS485; virtual for LoRa
  SensorTypeID type_id;
  uint8_t      address;        // Modbus address
  char         instance[4];    // "01"-"16"
  char         device_key[56]; // "SLXA1260004_Soil_RK520-01"
  bool         is_lora;
  char         lora_node[8];   // "N01", "N02", ...
};

// ── Reserved Entry (disconnected, kept for address restoration) ───────────
struct ReservedEntry {
  bool         occupied;
  uint8_t      address;
  SensorTypeID type_id;
  char         instance[4];
  uint8_t      last_port;
  uint32_t     reserved_at; // unix seconds
};

static SensorEntry   _active[MAX_SENSOR_SLOTS]   = {};
static ReservedEntry _reserved[MAX_SENSOR_SLOTS] = {};
static SemaphoreHandle_t _regMutex = nullptr;

// ── Build ThingsBoard device key ──────────────────────────────────────────
static void _buildKey(SensorEntry& e) {
  const char* disp  = SENSOR_TYPES[e.type_id].display_name;
  const char* model = SENSOR_TYPES[e.type_id].model;
  // LoRa sensors use the same key format as local sensors — only instance differs
  snprintf(e.device_key, sizeof(e.device_key), "%s_%s_%s-%s",
           DEVICE_PREFIX, disp, model, e.instance);
}

// ── Persistence helpers ───────────────────────────────────────────────────
static void _saveActive() {
  File f = LittleFS.open(ACTIVE_PATH, "w");
  if (!f) return;
  DynamicJsonDocument doc(4096);
  JsonArray arr = doc.to<JsonArray>();
  for (auto& e : _active) {
    if (!e.occupied) continue;
    JsonObject o = arr.createNestedObject();
    o["slot"]      = e.slot;
    o["port"]      = e.port;
    o["type_id"]   = (int)e.type_id;
    o["address"]   = e.address;
    o["instance"]  = e.instance;
    o["is_lora"]   = e.is_lora;
    o["lora_node"] = e.lora_node;
  }
  serializeJson(doc, f);
  f.close();
}

static void _saveReserved() {
  File f = LittleFS.open(RESERVED_PATH, "w");
  if (!f) return;
  DynamicJsonDocument doc(4096);
  JsonArray arr = doc.to<JsonArray>();
  for (auto& r : _reserved) {
    if (!r.occupied) continue;
    JsonObject o = arr.createNestedObject();
    o["address"]     = r.address;
    o["type_id"]     = (int)r.type_id;
    o["instance"]    = r.instance;
    o["last_port"]   = r.last_port;
    o["reserved_at"] = r.reserved_at;
  }
  serializeJson(doc, f);
  f.close();
}

// ── Public API ────────────────────────────────────────────────────────────
inline void registryInit() {
  _regMutex = xSemaphoreCreateMutex();
  if (!LittleFS.begin(true)) {
    Serial.println("[REG] LittleFS format...");
    LittleFS.format();
    LittleFS.begin();
  }
  LittleFS.mkdir("/sensors");
  LittleFS.mkdir("/offline");
  memset(_active,   0, sizeof(_active));
  memset(_reserved, 0, sizeof(_reserved));
  for (int i = 0; i < MAX_SENSOR_SLOTS; i++) _active[i].slot = i;
}

inline void registryLoad() {
  if (xSemaphoreTake(_regMutex, portMAX_DELAY) != pdTRUE) return;

  File f = LittleFS.open(ACTIVE_PATH, "r");
  if (f) {
    DynamicJsonDocument doc(4096);
    if (!deserializeJson(doc, f)) {
      for (JsonObject o : doc.as<JsonArray>()) {
        int slot = o["slot"] | -1;
        if (slot < 0 || slot >= MAX_SENSOR_SLOTS) continue;
        SensorEntry& e = _active[slot];
        e.occupied = true;
        e.slot     = slot;
        e.port     = o["port"];
        e.type_id  = (SensorTypeID)(int)o["type_id"];
        e.address  = o["address"];
        strlcpy(e.instance,  o["instance"]  | "01", sizeof(e.instance));
        e.is_lora  = o["is_lora"] | false;
        strlcpy(e.lora_node, o["lora_node"] | "",   sizeof(e.lora_node));
        _buildKey(e);
      }
    }
    f.close();
  }

  f = LittleFS.open(RESERVED_PATH, "r");
  if (f) {
    DynamicJsonDocument doc(4096);
    if (!deserializeJson(doc, f)) {
      uint32_t now_s = (uint32_t)(millis() / 1000);
      int idx = 0;
      for (JsonObject o : doc.as<JsonArray>()) {
        if (idx >= MAX_SENSOR_SLOTS) break;
        uint32_t ra = o["reserved_at"] | 0U;
        if (ra > 0 && (now_s - ra) > RESERVED_MAX_SECS) continue; // expired
        ReservedEntry& r = _reserved[idx++];
        r.occupied    = true;
        r.address     = o["address"];
        r.type_id     = (SensorTypeID)(int)o["type_id"];
        strlcpy(r.instance, o["instance"] | "01", sizeof(r.instance));
        r.last_port   = o["last_port"] | 0;
        r.reserved_at = ra;
      }
    }
    f.close();
  }

  xSemaphoreGive(_regMutex);
}

// Add (or update if same port) a sensor. Returns slot index or -1.
inline int registryAdd(uint8_t port, SensorTypeID type, uint8_t addr,
                        const char* instance,
                        bool is_lora = false, const char* lora_node = "") {
  if (xSemaphoreTake(_regMutex, portMAX_DELAY) != pdTRUE) return -1;

  // Find existing slot for this port (update) or first free slot
  int slot = -1;
  for (int i = 0; i < MAX_SENSOR_SLOTS; i++) {
    if (_active[i].occupied && _active[i].port == port &&
        _active[i].is_lora == is_lora) { slot = i; break; }
  }
  if (slot < 0) {
    for (int i = 0; i < MAX_SENSOR_SLOTS; i++)
      if (!_active[i].occupied) { slot = i; break; }
  }
  if (slot < 0) { xSemaphoreGive(_regMutex); return -1; }

  SensorEntry& e = _active[slot];
  e.occupied  = true;
  e.slot      = slot;
  e.port      = port;
  e.type_id   = type;
  e.address   = addr;
  strlcpy(e.instance,  instance,  sizeof(e.instance));
  e.is_lora   = is_lora;
  strlcpy(e.lora_node, lora_node, sizeof(e.lora_node));
  _buildKey(e);

  // Remove from reserved if present
  for (auto& r : _reserved)
    if (r.occupied && r.address == addr && r.type_id == type) { r.occupied = false; break; }

  _saveActive();
  _saveReserved();
  xSemaphoreGive(_regMutex);
  Serial.printf("[REG] Added: slot=%d key=%s\n", slot, _active[slot].device_key);
  return slot;
}

// Move a port's sensor to reserved (unplugged)
inline void registryRemove(uint8_t port) {
  if (xSemaphoreTake(_regMutex, portMAX_DELAY) != pdTRUE) return;
  for (auto& e : _active) {
    if (!e.occupied || e.port != port) continue;
    // Move to first free reserved slot
    for (auto& r : _reserved) {
      if (!r.occupied) {
        r.occupied    = true;
        r.address     = e.address;
        r.type_id     = e.type_id;
        strlcpy(r.instance, e.instance, sizeof(r.instance));
        r.last_port   = port;
        r.reserved_at = (uint32_t)(millis() / 1000);
        break;
      }
    }
    e.occupied = false;
    Serial.printf("[REG] Removed port %d → reserved\n", port);
    break;
  }
  _saveActive();
  _saveReserved();
  xSemaphoreGive(_regMutex);
}

inline SensorEntry* registryGetBySlot(int slot) {
  if (slot < 0 || slot >= MAX_SENSOR_SLOTS) return nullptr;
  return _active[slot].occupied ? &_active[slot] : nullptr;
}

inline SensorEntry* registryGetByPort(uint8_t port) {
  for (auto& e : _active)
    if (e.occupied && e.port == port) return &e;
  return nullptr;
}

// Find an active LoRa sensor by node+type+address
inline SensorEntry* registryGetLoRaSensor(const char* node, SensorTypeID type, uint8_t addr) {
  for (auto& e : _active)
    if (e.occupied && e.is_lora &&
        strcmp(e.lora_node, node) == 0 &&
        e.type_id == type && e.address == addr) return &e;
  return nullptr;
}

inline bool registryAddressUsed(uint8_t addr) {
  for (auto& e : _active)
    if (e.occupied && e.address == addr && !e.is_lora) return true;
  return false;
}

inline ReservedEntry* registryFindReserved(uint8_t addr, SensorTypeID type) {
  for (auto& r : _reserved)
    if (r.occupied && r.address == addr && r.type_id == type) return &r;
  return nullptr;
}

inline int registryCountType(SensorTypeID type) {
  int n = 0;
  for (auto& e : _active)
    if (e.occupied && e.type_id == type) n++;
  return n;
}

// Find first address in range not used by any active local sensor
inline uint8_t registryFindFreeAddress(SensorTypeID type) {
  const SensorTypeDef& t = SENSOR_TYPES[type];
  for (uint8_t a = t.addr_min; a <= t.addr_max; a++)
    if (!registryAddressUsed(a)) return a;
  return 0; // none available
}

// Fill array with all occupied active entries, return count
inline int registryGetAll(SensorEntry* out[], int max_count) {
  int n = 0;
  for (auto& e : _active)
    if (e.occupied && n < max_count) out[n++] = &e;
  return n;
}
