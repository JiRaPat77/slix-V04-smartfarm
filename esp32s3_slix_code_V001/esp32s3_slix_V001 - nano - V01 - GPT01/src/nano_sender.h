// Nano Sender — pack all sensor readings + push DATA to gateway via LoRa.
//
// Strategy:
//   • Try single packed payload (all sensors in 1 LoRa packet)
//   • If > NANO_LORA_SOFT_MAX_BYTES → auto-split per port (still sent
//     back-to-back within ~1 sec → effectively "together")
//
// Payload format (multi-sensor packed):
//   [DATA] <nodeId>|{"<port>":{"t":"<type>","i":"<inst>","d":{<json>}},...}
//
// Payload format (single-port split fallback):
//   [DATA] <nodeId>|{"<port>":{"t":"<type>","i":"<inst>","d":{<json>}}}
//
// Both formats share the same parser on gateway side.
#pragma once
#include <Arduino.h>
#include <math.h>
#include "lora_protocol.h"
#include "nano_config.h"
#include "nano_pairing.h"
#include "nano_sensor.h"
#include "sensor_types.h"

static uint32_t _nanoLastSend     = 0;
static uint32_t _nanoLastAck      = 0;
static int      _nanoMissedAcks   = 0;

// ── Build the JSON value for one port: {"t":"...","i":"...","d":{...}} ──
// Returns chars written (not including terminator), 0 if invalid.
static int _buildPortJson(char* out, size_t maxLen, int idx) {
  const PortConfig& cfg = PORT_CONFIG[idx];
  const NanoLastReading* lr = nanoLastReading(idx);
  if (!lr || !lr->valid) return 0;

  int p = snprintf(out, maxLen, "{\"t\":\"%s\",\"i\":\"%s\",\"d\":{",
                   SENSOR_TYPES[cfg.type].type_name,
                   cfg.instance);
  for (uint8_t f = 0; f < lr->data.field_count; f++) {
    p += snprintf(out + p, maxLen - p,
                  "%s\"%s\":%.2f",
                  f > 0 ? "," : "",
                  lr->data.fields[f].key,
                  lr->data.fields[f].val);
  }
  p += snprintf(out + p, maxLen - p, "}}");
  return p;
}

// ── Send all sensors in ONE LoRa packet ─────────────────────────────────
// Returns true on success, false if payload too big (caller falls back to split)
static bool _sendPacked() {
  char payload[NANO_LORA_SOFT_MAX_BYTES + 80];

  // Use ts from the first valid reading (all readings share one batch timestamp)
  uint64_t ts_ms = 0;
  for (int i = 0; i < PORT_CONFIG_COUNT; i++) {
    const NanoLastReading* lr = nanoLastReading(i);
    if (lr && lr->valid) { ts_ms = lr->ts_bkk_ms; break; }
  }

  // nodeId|{"ts":...,  — ts is Bangkok unix ms, matches gateway convention
  int p = snprintf(payload, sizeof(payload), "%s|{\"ts\":%llu,",
                   nanoNodeId(), (unsigned long long)ts_ms);
  bool any = false;
  bool first = true;

  char portBuf[160];

  for (int i = 0; i < PORT_CONFIG_COUNT; i++) {
    int n = _buildPortJson(portBuf, sizeof(portBuf), i);
    if (n == 0) continue;

    int needed = (first ? 0 : 1) /*comma*/ +
                 strlen("\"") + 4 /* port up to 12 + "\"" + ":" */ +
                 n + 1 /*closing brace check*/;
    if ((p + needed) > (int)NANO_LORA_SOFT_MAX_BYTES) {
      Serial.printf("[SEND] Packed payload would exceed %d bytes — fallback to split\n",
                    NANO_LORA_SOFT_MAX_BYTES);
      return false;
    }

    p += snprintf(payload + p, sizeof(payload) - p,
                  "%s\"%u\":%s",
                  first ? "" : ",",
                  PORT_CONFIG[i].port,
                  portBuf);
    first = false;
    any = true;
  }

  if (!any) {
    Serial.println("[SEND] No valid readings — skip");
    return true;   // not a failure, just nothing to send
  }

  p += snprintf(payload + p, sizeof(payload) - p, "}");

  loraSendMessage("DATA", payload);
  Serial.printf("[SEND] DATA packed (%d bytes): %s\n", p, payload);
  return true;
}

// ── Fallback: send one packet per port ──────────────────────────────────
static void _sendSplit() {
  char payload[300];
  char portBuf[160];

  for (int i = 0; i < PORT_CONFIG_COUNT; i++) {
    int n = _buildPortJson(portBuf, sizeof(portBuf), i);
    if (n == 0) continue;
    const NanoLastReading* lr = nanoLastReading(i);
    uint64_t ts_ms = lr ? lr->ts_bkk_ms : 0;

    int p = snprintf(payload, sizeof(payload),
                     "%s|{\"ts\":%llu,\"%u\":%s}",
                     nanoNodeId(), (unsigned long long)ts_ms,
                     PORT_CONFIG[i].port, portBuf);

    loraSendMessage("DATA", payload);
    Serial.printf("[SEND] DATA split p%u (%d bytes): %s\n",
                  PORT_CONFIG[i].port, p, payload);
    delay(150);   // small gap to avoid LoRa contention
  }
}

// ── Public API ───────────────────────────────────────────────────────────
inline void nanoSendNow() {
  if (!nanoIsPaired()) return;
  if (PORT_CONFIG_COUNT == 0) return;

  if (!_sendPacked()) {
    _sendSplit();
  }
}

inline void nanoSendLoop() {
  if (!nanoIsPaired()) return;
  uint32_t now = millis();
  if (now - _nanoLastSend < NANO_SEND_INTERVAL_MS) return;
  _nanoLastSend = now;
  nanoSendNow();
}

// ── Inbound message handlers ─────────────────────────────────────────────
static void _trimToken(char*& p) {
  while (*p == ' ') p++;
  char* q = p;
  while (*q && *q != ' ' && *q != '\r' && *q != '\n') q++;
  *q = '\0';
}

inline void nanoHandleAckData(char* payload) {
  _trimToken(payload);
  // ack may include port info "N1|<port>" — accept either form
  char* sep = strchr(payload, '|');
  if (sep) *sep = '\0';
  if (strcmp(payload, nanoNodeId()) != 0) return;
  _nanoLastAck = millis();
  _nanoMissedAcks = 0;
  Serial.println("[SEND] ACK_DATA ✓");
}

// Optional: gateway sends [REQUEST <id>] → reply immediately with packed DATA
inline void nanoHandleRequest(char* payload) {
  _trimToken(payload);
  if (strcmp(payload, nanoNodeId()) != 0) return;
  Serial.println("[SEND] REQUEST received → replying now");
  nanoSendNow();
}

// ── Watchdog: re-init LoRa if no ACK for too long ────────────────────────
inline void nanoSenderWatchdog() {
  if (!nanoIsPaired()) return;
  if (_nanoLastAck == 0) return;
  uint32_t now = millis();
  if (now - _nanoLastAck > NANO_RX_WATCHDOG_MS) {
    Serial.println("[WD] No ACK for too long — re-init LoRa radio");
    LoRa.end();
    delay(50);
    loraInit();
    _nanoLastAck = now;
  }
}
