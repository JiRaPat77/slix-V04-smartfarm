// LoRa Gateway — pairing + polling + RX dispatch
//
// Behaviour summary:
//   • LoRa is initialised at boot in continuous RX mode.
//   • loraGatewayLoop() must be called frequently from main loop().
//   • RX: parse incoming packet → dispatch by command:
//       - HELLO       (only handled in pairing mode → ASSIGN reply)
//       - ACK_ASSIGN  (paired confirmation)
//       - DATA        (update node registry)
//   • Pairing mode: triggered by loraPairingToggle() (e.g. from button).
//     Lasts LORA_PAIRING_TIMEOUT_MS, broadcasts DISCOVER every
//     LORA_DISCOVER_INTERVAL_MS, accepts HELLO replies.
//   • Polling: every LORA_POLL_INTERVAL_MS, send REQUEST to next paired
//     node round-robin. Node should reply with DATA within ~5s.
//
// Tunables — change here and rebuild.
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <RTClib.h>
#include "lora_protocol.h"
#include "lora_registry.h"

extern RTC_DS3231 rtc;

// ── Tunable timing ───────────────────────────────────────────────────────
#define LORA_PAIRING_TIMEOUT_MS    (3UL * 60UL * 1000UL)   // 3 minutes
#define LORA_DISCOVER_INTERVAL_MS  5000UL                   // re-broadcast DISCOVER
#define LORA_POLL_INTERVAL_MS      30000UL                  // poll one node every 30s
#define LORA_DATA_TIMEOUT_MS       5000UL                   // wait for DATA after REQUEST

static bool          _loraPairing      = false;
static uint32_t      _loraPairStart    = 0;
static uint32_t      _loraLastDiscover = 0;
static uint32_t      _loraLastPoll     = 0;
static int           _loraPollIdx      = 0;

// ── Pairing control (call from button handler) ───────────────────────────
inline bool loraPairingActive() { return _loraPairing; }

inline void loraPairingEnter() {
  if (!loraAvailable()) return;
  _loraPairing      = true;
  _loraPairStart    = millis();
  _loraLastDiscover = 0;
  Serial.println("[LORA-PAIR] ENTER pairing mode (3 min)");
}

inline void loraPairingExit() {
  if (!_loraPairing) return;
  _loraPairing = false;
  Serial.println("[LORA-PAIR] EXIT pairing mode");
}

inline void loraPairingToggle() {
  if (_loraPairing) loraPairingExit();
  else              loraPairingEnter();
}

// ── Internal: handle inbound HELLO during pairing ────────────────────────
static void _handleHello(char* payload) {
  // payload format: temp_id|type_name|model
  char* p1 = strchr(payload, '|');
  if (!p1) return;
  *p1 = '\0';
  char* p2 = strchr(p1 + 1, '|');
  if (!p2) return;
  *p2 = '\0';
  const char* temp_id   = payload;
  const char* type_name = p1 + 1;
  const char* model     = p2 + 1;

  Serial.printf("[LORA-PAIR] HELLO temp=%s type=%s model=%s\n",
                temp_id, type_name, model);

  // Already paired? (rare — node restart with old temp_id) — ignore
  if (loraRegistryFind(temp_id)) return;

  SensorTypeID type = sensorTypeFromName(type_name);
  if (type == ST_UNKNOWN) {
    Serial.printf("[LORA-PAIR] Unknown type '%s' — ignored\n", type_name);
    return;
  }

  LoRaNode* n = loraRegistryAllocate();
  if (!n) {
    Serial.println("[LORA-PAIR] Registry full — pairing rejected");
    return;
  }
  n->type = type;
  strlcpy(n->model, model, sizeof(n->model));

  // Send ASSIGN reply: temp_id|assigned_id
  char reply[64];
  snprintf(reply, sizeof(reply), "%s|%s", temp_id, n->id);
  loraSendMessage("ASSIGN", reply);
  Serial.printf("[LORA-PAIR] ASSIGN %s → %s\n", temp_id, n->id);
}

static void _handleAckAssign(char* payload) {
  // payload = assigned_id (no separator)
  char* sp = strchr(payload, ' ');
  if (sp) *sp = '\0';
  Serial.printf("[LORA-PAIR] ACK_ASSIGN from %s — paired & saved\n", payload);
  loraRegistrySave();
}

static void _handleData(char* payload, int rssi) {
  // payload format: id|<json>
  char* sep = strchr(payload, '|');
  if (!sep) return;
  *sep = '\0';
  const char* id   = payload;
  const char* json = sep + 1;

  LoRaNode* n = loraRegistryFind(id);
  if (!n) {
    Serial.printf("[LORA-RX] DATA from unknown node '%s' — ignored\n", id);
    return;
  }

  DynamicJsonDocument doc(512);
  DeserializationError err = deserializeJson(doc, json);
  if (err) {
    Serial.printf("[LORA-RX] DATA parse error from %s: %s\n", id, err.c_str());
    return;
  }
  JsonObject vals = doc.as<JsonObject>();
  loraRegistryUpdateData(id, vals, rtc.now().unixtime(), rssi);

  // ACK
  loraSendMessage("ACK_DATA", id);
  Serial.printf("[LORA-RX] DATA from %s rssi=%d, %d field(s) updated\n",
                id, rssi, n->field_count);
}

// ── Main loop entry ──────────────────────────────────────────────────────
inline void loraGatewayLoop() {
  if (!loraAvailable()) return;

  uint32_t now = millis();

  // 1) Pairing mode timing
  if (_loraPairing) {
    if (now - _loraPairStart >= LORA_PAIRING_TIMEOUT_MS) {
      loraPairingExit();
    } else if (now - _loraLastDiscover >= LORA_DISCOVER_INTERVAL_MS) {
      _loraLastDiscover = now;
      loraSendMessage("DISCOVER", "-");
      Serial.println("[LORA-PAIR] DISCOVER broadcast");
    }
  }

  // 2) RX poll
  static char rxbuf[256];
  int rssi = 0;
  size_t n = loraReceiveMessage(rxbuf, sizeof(rxbuf), &rssi, nullptr);
  if (n > 0) {
    char *cmd = nullptr, *payload = nullptr;
    if (loraParseMessage(rxbuf, &cmd, &payload)) {
      if      (_loraPairing && strcmp(cmd, "HELLO") == 0)      _handleHello(payload);
      else if (strcmp(cmd, "ACK_ASSIGN") == 0)                  _handleAckAssign(payload);
      else if (strcmp(cmd, "DATA") == 0)                        _handleData(payload, rssi);
      else {
        // Unknown / out-of-context command
      }
    }
  }

  // 3) Polling: REQUEST next paired node every 30s (only when not pairing)
  if (!_loraPairing && (now - _loraLastPoll >= LORA_POLL_INTERVAL_MS)) {
    _loraLastPoll = now;
    int total = loraRegistryCount();
    if (total > 0) {
      // Step through slots until we find a used one
      for (int i = 0; i < LORA_MAX_NODES; i++) {
        int idx = (_loraPollIdx + i) % LORA_MAX_NODES;
        LoRaNode* node = loraRegistrySlot(idx);
        if (node) {
          loraSendMessage("REQUEST", node->id);
          Serial.printf("[LORA-POLL] REQUEST %s\n", node->id);
          _loraPollIdx = (idx + 1) % LORA_MAX_NODES;
          break;
        }
      }
    }
  }
}
