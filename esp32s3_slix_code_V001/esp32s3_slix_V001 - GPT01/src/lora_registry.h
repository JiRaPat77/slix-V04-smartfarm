// LoRa Node Registry — each paired Nano can have 1-6 sensors.
//
// Persistent: id only (SD/LittleFS /lora_nodes.json).
// Runtime:    sensor types + last data (filled from first DATA packet).
//
// Nano sends: [DATA] N1|{"ts":ms,"1":{"t":"soil","i":"01","d":{fields...}},...}
//   "ts"  = Bangkok unix ms (key literal "ts")
//   "1"   = port number string (numeric key)
//   "t"   = sensor type name (matches sensorTypeFromName)
//   "i"   = instance string ("01", "02" ...)
//   "d"   = flat field object matching sensorFieldKey() keys
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <SD.h>
#include "sensor_types.h"
#include "sd_card.h"

#define LORA_MAX_NODES            16
#define LORA_NODE_ID_LEN           8
#define LORA_MAX_SENSORS_PER_NODE  6   // max sensors per one nano
#define LORA_REGISTRY_PATH        "/lora_nodes.json"

struct LoRaNodeSensor {
  bool          used;
  uint8_t       port;                       // port number on nano (1-12)
  SensorTypeID  type;
  char          model[16];
  char          instance[8];               // "01", "02" ...
  uint32_t      last_seen;                 // unix sec
  bool          data_valid;
  uint8_t       field_count;
  float         fields[MAX_SENSOR_FIELDS];
};

struct LoRaNode {
  bool             used;
  char             id[LORA_NODE_ID_LEN];   // "N1" .. "N16"
  uint32_t         last_seen;              // most recent contact unix sec
  uint32_t         last_rx_ms;             // millis() of last DATA received
  float            battery;               // V, NAN if not reported
  int              last_rssi;
  int              sensor_count;
  LoRaNodeSensor   sensors[LORA_MAX_SENSORS_PER_NODE];
};

static LoRaNode _loraNodes[LORA_MAX_NODES] = {};

// ── File I/O ─────────────────────────────────────────────────────────────
static bool _lrExists(const char* p) {
  return sdAvailable() ? SD.exists(p) : LittleFS.exists(p);
}
static File _lrOpenRead(const char* p) {
  return sdAvailable() ? SD.open(p, FILE_READ) : LittleFS.open(p, "r");
}
static File _lrOpenWrite(const char* p) {
  return sdAvailable() ? SD.open(p, FILE_WRITE) : LittleFS.open(p, "w");
}

// ── Public API ────────────────────────────────────────────────────────────
inline int loraRegistryCount() {
  int n = 0;
  for (int i = 0; i < LORA_MAX_NODES; i++) if (_loraNodes[i].used) n++;
  return n;
}

inline LoRaNode* loraRegistryFind(const char* id) {
  if (!id) return nullptr;
  for (int i = 0; i < LORA_MAX_NODES; i++)
    if (_loraNodes[i].used && strcmp(_loraNodes[i].id, id) == 0)
      return &_loraNodes[i];
  return nullptr;
}

inline LoRaNode* loraRegistrySlot(int idx) {
  if (idx < 0 || idx >= LORA_MAX_NODES) return nullptr;
  return _loraNodes[idx].used ? &_loraNodes[idx] : nullptr;
}

inline LoRaNode* loraRegistryAllocate() {
  bool used_ids[LORA_MAX_NODES + 1] = {};
  for (int i = 0; i < LORA_MAX_NODES; i++)
    if (_loraNodes[i].used && _loraNodes[i].id[0] == 'N') {
      int n = atoi(_loraNodes[i].id + 1);
      if (n >= 1 && n <= LORA_MAX_NODES) used_ids[n] = true;
    }
  int newId = 0;
  for (int i = 1; i <= LORA_MAX_NODES; i++) if (!used_ids[i]) { newId = i; break; }
  if (newId == 0) return nullptr;

  for (int i = 0; i < LORA_MAX_NODES; i++) {
    if (!_loraNodes[i].used) {
      memset(&_loraNodes[i], 0, sizeof(LoRaNode));
      _loraNodes[i].used = true;
      snprintf(_loraNodes[i].id, LORA_NODE_ID_LEN, "N%d", newId);
      _loraNodes[i].battery = NAN;
      return &_loraNodes[i];
    }
  }
  return nullptr;
}

// ── Save/Load (persist node identities only) ─────────────────────────────
inline void loraRegistrySave() {
  DynamicJsonDocument doc(2048);
  JsonArray arr = doc.createNestedArray("nodes");
  for (int i = 0; i < LORA_MAX_NODES; i++) {
    if (!_loraNodes[i].used) continue;
    JsonObject o = arr.createNestedObject();
    o["id"] = _loraNodes[i].id;
  }
  File f = _lrOpenWrite(LORA_REGISTRY_PATH);
  if (!f) { Serial.println("[LORA-REG] Save FAILED"); return; }
  serializeJson(doc, f);
  f.close();
  Serial.printf("[LORA-REG] Saved %d node(s)\n", loraRegistryCount());
}

inline void loraRegistryLoad() {
  memset(_loraNodes, 0, sizeof(_loraNodes));
  if (!_lrExists(LORA_REGISTRY_PATH)) {
    Serial.println("[LORA-REG] No registry — starting empty");
    return;
  }
  File f = _lrOpenRead(LORA_REGISTRY_PATH);
  if (!f) return;

  DynamicJsonDocument doc(2048);
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) { Serial.printf("[LORA-REG] Parse error: %s\n", err.c_str()); return; }

  JsonArray arr = doc["nodes"].as<JsonArray>();
  int idx = 0;
  for (JsonObject o : arr) {
    if (idx >= LORA_MAX_NODES) break;
    const char* id = o["id"] | "";
    if (!id[0]) continue;
    _loraNodes[idx].used = true;
    strlcpy(_loraNodes[idx].id, id, LORA_NODE_ID_LEN);
    _loraNodes[idx].battery = NAN;
    idx++;
  }
  Serial.printf("[LORA-REG] Loaded %d node(s)\n", loraRegistryCount());
}

// ── Called by _handleData() to upsert sensor data ───────────────────────
// port: nano's physical port number, ts: Bangkok unix seconds from nano's RTC
inline void loraRegistryUpdateSensor(LoRaNode* n, uint8_t port,
                                      SensorTypeID type, const char* instance,
                                      JsonObject dataFields,
                                      uint32_t ts_bkk_sec, int rssi) {
  // Find existing slot for this port, or add new
  LoRaNodeSensor* s = nullptr;
  for (int i = 0; i < n->sensor_count; i++)
    if (n->sensors[i].port == port) { s = &n->sensors[i]; break; }

  if (!s && n->sensor_count < LORA_MAX_SENSORS_PER_NODE) {
    s = &n->sensors[n->sensor_count++];
    s->port = port;
    s->type = type;
    strlcpy(s->model, SENSOR_TYPES[type].model, sizeof(s->model));
    strlcpy(s->instance, instance, sizeof(s->instance));
    Serial.printf("[LORA-REG] Node %s: new sensor port=%d type=%s instance=%s\n",
                  n->id, port, SENSOR_TYPES[type].type_name, instance);
  }
  if (!s) return;

  s->used      = true;
  s->last_seen = ts_bkk_sec;
  s->data_valid = true;
  s->field_count = 0;

  for (uint8_t f = 0; f < MAX_SENSOR_FIELDS; f++) {
    const char* key = sensorFieldKey(type, f);
    if (!key) break;
    if (dataFields.containsKey(key)) {
      s->fields[f] = dataFields[key].as<float>();
      s->field_count = f + 1;
    }
  }

  n->last_seen = ts_bkk_sec;
  n->last_rssi = rssi;
}
