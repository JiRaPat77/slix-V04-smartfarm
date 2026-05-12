// Nano Pairing — persistent node ID via NVS Preferences
//
// Boot sequence:
//   1. Generate temp_id from ESP32 MAC (unique per chip — guaranteed)
//   2. Read assigned_id from NVS:
//        - if exists → already paired, skip pairing
//        - if empty  → broadcast HELLO every NANO_HELLO_INTERVAL_MS
//   3. On [ASSIGN tempId|assignedId] match: save assignedId, become paired
//
// Factory reset: clear NVS → next boot will be unpaired
#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include "lora_protocol.h"
#include "nano_config.h"
#include "sensor_types.h"

#define NANO_NVS_NAMESPACE   "nano"
#define NANO_NVS_KEY_ID      "node_id"
#define NANO_TEMP_ID_LEN     12
#define NANO_NODE_ID_LEN     12

static char     _nanoTempId[NANO_TEMP_ID_LEN] = {};
static char     _nanoNodeId[NANO_NODE_ID_LEN] = {};   // "" = unpaired
static bool     _nanoPaired       = false;
static uint32_t _nanoLastHello    = 0;

// ── Public API ───────────────────────────────────────────────────────────
inline const char* nanoTempId()  { return _nanoTempId; }
inline const char* nanoNodeId()  { return _nanoNodeId; }
inline bool        nanoIsPaired(){ return _nanoPaired; }

// Generate temp_id from chip MAC. Same MAC = same temp_id, every boot.
static void _genTempId() {
  uint64_t mac = ESP.getEfuseMac();
  // Lower 32 bits of MAC, hex — gives "T" + 8 hex chars = "TF8A29C40"
  snprintf(_nanoTempId, sizeof(_nanoTempId), "T%08X",
           (uint32_t)(mac & 0xFFFFFFFFULL));
}

// Save assigned ID to NVS (persistent across reboot)
static void _nanoSaveId(const char* id) {
  Preferences p;
  p.begin(NANO_NVS_NAMESPACE, false);
  p.putString(NANO_NVS_KEY_ID, id);
  p.end();
}

inline void nanoPairingInit() {
  _genTempId();

  Preferences p;
  p.begin(NANO_NVS_NAMESPACE, true);            // read-only
  String stored = p.getString(NANO_NVS_KEY_ID, "");
  p.end();

  if (stored.length() > 0) {
    strlcpy(_nanoNodeId, stored.c_str(), sizeof(_nanoNodeId));
    _nanoPaired = true;
    Serial.printf("[PAIR] Loaded id=%s from NVS (paired) tempId=%s\n",
                  _nanoNodeId, _nanoTempId);
  } else {
    _nanoPaired = false;
    Serial.printf("[PAIR] No stored id — UNPAIRED (tempId=%s)\n", _nanoTempId);
    Serial.println("[PAIR] Will broadcast HELLO every 5s until ASSIGN received");
  }
}

// Wipe NVS → restart → boot will be unpaired
inline void nanoFactoryReset() {
  Preferences p;
  p.begin(NANO_NVS_NAMESPACE, false);
  p.clear();
  p.end();
  Serial.println("[PAIR] FACTORY RESET — restarting in 1s");
  delay(1000);
  ESP.restart();
}

// Call from main loop. Sends HELLO periodically while unpaired.
inline void nanoPairingLoop() {
  if (_nanoPaired) return;

  uint32_t now = millis();
  if (now - _nanoLastHello < NANO_HELLO_INTERVAL_MS) return;
  _nanoLastHello = now;

  // [HELLO] tempId  — sensor types learned by gateway from first DATA, not HELLO
  loraSendMessage("HELLO", _nanoTempId);
  Serial.printf("[PAIR] HELLO → %s\n", _nanoTempId);
}

// Called when [ASSIGN tempId|assignedId] received from gateway
inline void nanoHandleAssign(char* payload) {
  if (_nanoPaired) return;       // already paired — ignore (shouldn't happen)

  // Parse: tempId|assignedId
  char* sep = strchr(payload, '|');
  if (!sep) return;
  *sep = '\0';
  const char* tempId   = payload;
  const char* assigned = sep + 1;

  // Strip trailing whitespace/newline from assigned
  char* p = (char*)assigned;
  while (*p && *p != ' ' && *p != '\r' && *p != '\n') p++;
  *p = '\0';

  // Is this ASSIGN for me?
  if (strcmp(tempId, _nanoTempId) != 0) {
    Serial.printf("[PAIR] ASSIGN for %s (not me, ignored)\n", tempId);
    return;
  }

  // Save & enter paired state
  strlcpy(_nanoNodeId, assigned, sizeof(_nanoNodeId));
  _nanoSaveId(assigned);
  _nanoPaired = true;

  loraSendMessage("ACK_ASSIGN", assigned);
  Serial.printf("[PAIR] PAIRED as %s — saved to NVS\n", assigned);
}
