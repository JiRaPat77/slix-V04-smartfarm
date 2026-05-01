// LoRa Node Registry — persistent storage of paired nodes.
//
// Each entry represents one Nano (LoRa node) that's been paired with this
// gateway. Conceptually each node = a virtual sensor "plugged" into a
// virtual port on the gateway.
//
// Storage backend: SD card if available, else LittleFS.
// File: /lora_nodes.json
//
// Identity is persistent (id, type, model). Runtime data (last_seen,
// last_data, battery) is in-memory only — refreshed each time node sends.
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <SD.h>
#include "sensor_types.h"
#include "sd_card.h"

#define LORA_MAX_NODES        16
#define LORA_NODE_ID_LEN       8       // "N1".."N16" + null
#define LORA_REGISTRY_PATH    "/lora_nodes.json"

struct LoRaNode {
  bool          used;
  char          id[LORA_NODE_ID_LEN];   // "N1", "N2", ...
  SensorTypeID  type;
  char          model[16];              // "RK520", "RS-WS", ...
  // Runtime (not persisted)
  uint32_t      last_seen;              // unix sec, 0 = never
  bool          data_valid;
  uint8_t       field_count;
  float         fields[MAX_SENSOR_FIELDS];
  float         battery;                // V, NAN if not reported
  int           last_rssi;
};

static LoRaNode _loraNodes[LORA_MAX_NODES] = {};

// ── File I/O abstraction (SD or LittleFS) ────────────────────────────────
static bool _lrExists(const char* p) {
  if (sdAvailable()) return SD.exists(p);
  return LittleFS.exists(p);
}
static File _lrOpenRead(const char* p) {
  if (sdAvailable()) return SD.open(p, FILE_READ);
  return LittleFS.open(p, "r");
}
static File _lrOpenWrite(const char* p) {
  if (sdAvailable()) return SD.open(p, FILE_WRITE);
  return LittleFS.open(p, "w");
}

// ── Public API ───────────────────────────────────────────────────────────

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

// Find next free slot and assign next available node ID ("N1".."N16")
// Caller must populate type/model then call loraRegistrySave().
inline LoRaNode* loraRegistryAllocate() {
  // Used IDs bitmap
  bool used_id[LORA_MAX_NODES + 1] = {};
  for (int i = 0; i < LORA_MAX_NODES; i++) {
    if (_loraNodes[i].used && _loraNodes[i].id[0] == 'N') {
      int n = atoi(_loraNodes[i].id + 1);
      if (n >= 1 && n <= LORA_MAX_NODES) used_id[n] = true;
    }
  }
  // Pick lowest free ID
  int newId = 0;
  for (int i = 1; i <= LORA_MAX_NODES; i++) if (!used_id[i]) { newId = i; break; }
  if (newId == 0) return nullptr;

  // Find free slot
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

inline bool loraRegistryRemove(const char* id) {
  LoRaNode* n = loraRegistryFind(id);
  if (!n) return false;
  n->used = false;
  return true;
}

// Persist identity-only fields (id/type/model) to JSON file
inline void loraRegistrySave() {
  DynamicJsonDocument doc(2048);
  JsonArray arr = doc.createNestedArray("nodes");
  for (int i = 0; i < LORA_MAX_NODES; i++) {
    if (!_loraNodes[i].used) continue;
    JsonObject o = arr.createNestedObject();
    o["id"]    = _loraNodes[i].id;
    o["type"]  = SENSOR_TYPES[_loraNodes[i].type].type_name;
    o["model"] = _loraNodes[i].model;
  }

  File f = _lrOpenWrite(LORA_REGISTRY_PATH);
  if (!f) {
    Serial.println("[LORA-REG] Save failed (open)");
    return;
  }
  serializeJson(doc, f);
  f.close();
  Serial.printf("[LORA-REG] Saved %d node(s)\n", loraRegistryCount());
}

// Load registry from file. Safe to call on empty/missing file.
inline void loraRegistryLoad() {
  memset(_loraNodes, 0, sizeof(_loraNodes));
  if (!_lrExists(LORA_REGISTRY_PATH)) {
    Serial.println("[LORA-REG] No registry file — starting empty");
    return;
  }
  File f = _lrOpenRead(LORA_REGISTRY_PATH);
  if (!f) return;

  DynamicJsonDocument doc(2048);
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) {
    Serial.printf("[LORA-REG] Parse error: %s\n", err.c_str());
    return;
  }

  JsonArray arr = doc["nodes"].as<JsonArray>();
  int idx = 0;
  for (JsonObject o : arr) {
    if (idx >= LORA_MAX_NODES) break;
    LoRaNode& n = _loraNodes[idx];
    const char* id    = o["id"]    | "";
    const char* type  = o["type"]  | "";
    const char* model = o["model"] | "";
    if (!id[0] || !type[0]) continue;
    n.used = true;
    strlcpy(n.id, id, LORA_NODE_ID_LEN);
    strlcpy(n.model, model, sizeof(n.model));
    n.type = sensorTypeFromName(type);
    n.battery = NAN;
    idx++;
  }
  Serial.printf("[LORA-REG] Loaded %d node(s)\n", loraRegistryCount());
}

// Update runtime data after receiving DATA from a node
inline void loraRegistryUpdateData(const char* id, JsonObject vals,
                                    uint32_t now_unix, int rssi) {
  LoRaNode* n = loraRegistryFind(id);
  if (!n) return;
  n->last_seen = now_unix;
  n->last_rssi = rssi;
  n->data_valid = true;
  n->field_count = 0;

  // Map JSON fields to known field keys for this sensor type
  for (uint8_t f = 0; f < MAX_SENSOR_FIELDS; f++) {
    const char* key = sensorFieldKey(n->type, f);
    if (!key) break;
    if (vals.containsKey(key)) {
      n->fields[f] = vals[key].as<float>();
      n->field_count = f + 1;
    }
  }
  // Battery is optional, doesn't count as a sensor field
  if (vals.containsKey("battery")) n->battery = vals["battery"].as<float>();
}
