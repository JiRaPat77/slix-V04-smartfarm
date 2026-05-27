// LoRa Protocol — text-based message format
//
// Wire format: "[CMD] PAYLOAD"
//   PAYLOAD is whatever the cmd needs (may contain '|' separators or JSON).
//
// Commands:
//   GW broadcast :  [DISCOVER] -
//   Node → GW    :  [HELLO]      <temp_id>|<type_name>|<model>
//   GW → Node    :  [ASSIGN]     <temp_id>|<assigned_id>
//   Node → GW    :  [ACK_ASSIGN] <assigned_id>
//   GW → Node    :  [REQUEST]    <node_id>
//   Node → GW    :  [DATA]       <node_id>|<json_object_with_field_keys>
//   GW → Node    :  [ACK_DATA]   <node_id>
//
// JSON in [DATA] uses field keys identical to sensorFieldKey() in sensor_types.h
// e.g.  {"soil_temperature":25.5,"soil_moisture":70.0,"battery":3.7}
//
// LoRa radio params:
//   freq=433MHz, SF=7, BW=125kHz, sync=0x12 (default for sandeepmistry/LoRa)
#pragma once
#include <Arduino.h>
#include <SPI.h>
#include <LoRa.h>

#define LORA_FREQUENCY    433E6
#define LORA_BANDWIDTH    125E3
#define LORA_SF           7
#define LORA_TX_POWER     17
#define LORA_MAX_MSG      240

// LoRa pins on this board (SX1278 on HSPI)
#define LORA_RST_PIN      33
#define LORA_NSS_PIN      34
#define LORA_MOSI_PIN     35
#define LORA_SCK_PIN      36
#define LORA_MISO_PIN     37
#define LORA_DIO0_PIN     39

static SPIClass _loraSpi(HSPI);
static bool     _loraReady = false;

// Initialise SX1278. Returns true on success.
inline bool loraInit() {
  _loraSpi.begin(LORA_SCK_PIN, LORA_MISO_PIN, LORA_MOSI_PIN, LORA_NSS_PIN);
  LoRa.setSPI(_loraSpi);
  LoRa.setPins(LORA_NSS_PIN, LORA_RST_PIN, LORA_DIO0_PIN);

  if (!LoRa.begin(LORA_FREQUENCY)) {
    Serial.println("[LORA] begin() FAILED — module not detected");
    _loraReady = false;
    return false;
  }
  LoRa.setSpreadingFactor(LORA_SF);
  LoRa.setSignalBandwidth(LORA_BANDWIDTH);
  LoRa.setTxPower(LORA_TX_POWER);
  LoRa.enableCrc();
  LoRa.receive();   // continuous RX

  Serial.printf("[LORA] OK  freq=%.0fMHz  SF=%d  BW=%.0fkHz\n",
                (double)LORA_FREQUENCY / 1e6,
                LORA_SF,
                (double)LORA_BANDWIDTH / 1e3);
  _loraReady = true;
  return true;
}

inline bool loraAvailable() { return _loraReady; }

// Encode "[CMD] payload" into buf. Returns length written (excl. NUL).
inline size_t loraPackMessage(char* buf, size_t maxLen,
                              const char* cmd, const char* payload) {
  return snprintf(buf, maxLen, "[%s] %s", cmd, payload ? payload : "");
}

// Parse received message in-place: splits "[CMD] payload"
// Returns true if format valid; cmd_out/payload_out point inside `msg`.
// `msg` is modified (NUL terminator inserted at ']').
inline bool loraParseMessage(char* msg, char** cmd_out, char** payload_out) {
  if (!msg || msg[0] != '[') return false;
  char* end = strchr(msg, ']');
  if (!end) return false;
  *end = '\0';
  *cmd_out = msg + 1;
  char* p = end + 1;
  while (*p == ' ') p++;
  *payload_out = p;
  return true;
}

// Send a message — blocks until TX done (~50-200ms typical), then returns to RX.
inline void loraSendMessage(const char* cmd, const char* payload) {
  if (!_loraReady) return;
  char buf[LORA_MAX_MSG];
  size_t n = loraPackMessage(buf, sizeof(buf), cmd, payload);
  LoRa.beginPacket();
  LoRa.write((const uint8_t*)buf, n);
  LoRa.endPacket();
  LoRa.receive();
}

// Poll for incoming packet. Returns length copied into buf (0 if nothing).
inline size_t loraReceiveMessage(char* buf, size_t maxLen,
                                  int* rssi_out = nullptr,
                                  float* snr_out = nullptr) {
  if (!_loraReady) return 0;
  int sz = LoRa.parsePacket();
  if (sz <= 0) return 0;
  size_t n = 0;
  while (LoRa.available() && n < maxLen - 1) {
    int b = LoRa.read();
    if (b < 0) break;
    buf[n++] = (char)b;
  }
  buf[n] = '\0';
  if (rssi_out) *rssi_out = LoRa.packetRssi();
  if (snr_out)  *snr_out  = LoRa.packetSnr();
  return n;
}
