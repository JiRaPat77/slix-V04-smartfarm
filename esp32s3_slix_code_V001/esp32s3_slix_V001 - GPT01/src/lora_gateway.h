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

// Millis-based UTC clock (defined in main.cpp)
extern uint32_t _bootUtcSec;
extern uint32_t _bootMs;

// ── Timezone offset (Bangkok = UTC+7) ─────────────────────────────────────
#ifndef BANGKOK_OFFSET_SEC
#define BANGKOK_OFFSET_SEC  (7L * 3600L)
#endif

// ── Tunable timing ───────────────────────────────────────────────────────
#define LORA_PAIRING_TIMEOUT_MS    (3UL * 60UL * 1000UL)   // 3 minutes
#define LORA_DISCOVER_INTERVAL_MS  5000UL                   // re-broadcast DISCOVER
#define LORA_POLL_INTERVAL_MS      30000UL                  // poll one node every 30s
#define LORA_DATA_TIMEOUT_MS       5000UL                   // wait for DATA after REQUEST
#define LORA_WATCHDOG_REFRESH_MS   (5UL * 60UL * 1000UL)    // refresh radio rx mode every 5 min
#define LORA_WATCHDOG_REINIT_MS    (15UL * 60UL * 1000UL)   // full re-init if no rx for 15 min

static bool          _loraPairing      = false;
static uint32_t      _loraPairStart    = 0;
static uint32_t      _loraLastDiscover = 0;
static uint32_t      _loraLastPoll     = 0;
static int           _loraPollIdx      = 0;
static uint32_t      _loraLastRxMs     = 0;
static uint32_t      _loraLastRefresh  = 0;

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
// Nano sends: [HELLO] tempId   (sensor types learned later from first DATA)
static void _handleHello(char* payload) {
  // Strip trailing whitespace from tempId
  char* p = payload;
  while (*p && *p != ' ' && *p != '\r' && *p != '\n') p++;
  *p = '\0';
  const char* temp_id = payload;
  if (!temp_id[0]) return;

  Serial.printf("[LORA-PAIR] HELLO tempId=%s\n", temp_id);

  // Already have a node with this temp_id? ignore (already assigned)
  if (loraRegistryFind(temp_id)) return;

  LoRaNode* n = loraRegistryAllocate();
  if (!n) { Serial.println("[LORA-PAIR] Registry full"); return; }

  // Send ASSIGN: tempId|assignedId
  char reply[32];
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

// Parse: [DATA] nodeId|{"ts":ms,"<port>":{"t":"type","i":"inst","d":{fields}},...}
static void _handleData(char* payload, int rssi) {
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

  DynamicJsonDocument doc(1024);
  DeserializationError err = deserializeJson(doc, json);
  if (err) {
    Serial.printf("[LORA-RX] JSON parse error from %s: %s\n", id, err.c_str());
    return;
  }

  // Extract timestamp from nano (Bangkok unix ms → convert to UTC sec for storage)
  // Nano sends Bangkok time, gateway stores UTC for consistency
  uint64_t ts_ms = doc["ts"] | (uint64_t)0;
  // Fallback: use millis-based UTC (avoids DS3231 I2C glitch)
  uint32_t ts_utc_sec = ts_ms > 0 ? (uint32_t)(ts_ms / 1000ULL) - BANGKOK_OFFSET_SEC
                                   : _bootUtcSec + (uint32_t)((millis() - _bootMs) / 1000UL);

  // Optional battery voltage
  if (doc.containsKey("battery")) n->battery = doc["battery"].as<float>();

  // Iterate port entries: keys that parse as numeric = port numbers
  int updated = 0;
  for (JsonPair kv : doc.as<JsonObject>()) {
    const char* key = kv.key().c_str();
    // skip special keys
    if (strcmp(key, "ts") == 0 || strcmp(key, "battery") == 0) continue;

    uint8_t port = (uint8_t)atoi(key);
    if (port == 0) continue;               // not a port number

    JsonObject portObj = kv.value().as<JsonObject>();
    const char* typeName = portObj["t"] | "";
    const char* instance = portObj["i"] | "01";
    JsonObject  dataObj  = portObj["d"].as<JsonObject>();

    SensorTypeID type = sensorTypeFromName(typeName);
    if (type == ST_UNKNOWN) continue;

    loraRegistryUpdateSensor(n, port, type, instance, dataObj, ts_utc_sec, rssi);
    updated++;
  }

  n->last_rx_ms = millis();   // track when we last heard from this node
  loraSendMessage("ACK_DATA", id);
  Serial.printf("[LORA-RX] DATA %s rssi=%d ts=%u, %d port(s) updated\n",
                id, rssi, ts_utc_sec, updated);
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
    _loraLastRxMs = now;     // watchdog: เห็นแพ็กเก็ตล่าสุดเมื่อไหร่
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

  // 2.5) Watchdog — ยืนยันว่า radio ยังอยู่ใน RX mode + recover ถ้านิ่งนานผิดปกติ
  if (now - _loraLastRefresh >= LORA_WATCHDOG_REFRESH_MS) {
    _loraLastRefresh = now;
    LoRa.receive();   // no-op ถ้า rx อยู่แล้ว — recover ถ้า state เพี้ยน
  }
  if (loraRegistryCount() > 0 &&
      _loraLastRxMs > 0 &&
      (now - _loraLastRxMs) >= LORA_WATCHDOG_REINIT_MS) {
    Serial.println("[LORA-WD] No RX for 15 min — re-init radio");
    LoRa.end();
    delay(50);
    loraInit();
    _loraLastRxMs = now;
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
