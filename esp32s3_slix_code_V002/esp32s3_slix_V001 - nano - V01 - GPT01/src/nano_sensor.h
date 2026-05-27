// Nano Sensor I/O — MCP23017 power control + RS485 Modbus read
//
// Same hardware mapping as gateway: ports 1-4 on MCP1.OLATB, 5-8 on MCP1.OLATA,
// 9-12 on MCP2.OLATB. Lower nibble = power output, upper nibble = sensor-check
// input (not used for sending — gateway-only feature).
//
// API:
//   nanoSensorInit()       — init MCP23017, power on configured ports
//   nanoSensorReadAll()    — read every PORT_CONFIG entry into LastReading[]
//   nanoLastReading(idx)   — read latest cached value for PORT_CONFIG[idx]
#pragma once
#include <Arduino.h>
#include <Wire.h>
#include "nano_config.h"
#include "sensor_types.h"
#include "modbus_rtu.h"

// Bangkok timestamp — declared in main.cpp
extern RTC_DS3231 rtc_nano;
#define NANO_BANGKOK_OFFSET_SEC  (7L * 3600L)

// Millis-based UTC clock (avoids DS3231 I2C glitch on ESP32-S3)
extern uint32_t _nanoBootUtcSec;    // set once from RTC at boot
extern uint32_t _nanoBootMs;        // millis() at boot reference

struct NanoLastReading {
  bool        valid;
  SensorData  data;
  uint64_t    ts_bkk_ms;   // Bangkok unix milliseconds (for LoRa payload)
};
static NanoLastReading _nanoReadings[16] = {};

// ── MCP23017 helpers ─────────────────────────────────────────────────────
static void _mcpWrite(uint8_t addr, uint8_t reg, uint8_t val) {
  Wire.beginTransmission(addr);
  Wire.write(reg); Wire.write(val);
  Wire.endTransmission();
}
static uint8_t _mcpRead(uint8_t addr, uint8_t reg) {
  Wire.beginTransmission(addr); Wire.write(reg); Wire.endTransmission();
  Wire.requestFrom((int)addr, 1);
  return Wire.read();
}

// Power control (lower nibble of OLATA/OLATB) — mirrors gateway sensor_en_set()
static void _sensorPower(uint8_t port, bool on) {
  uint8_t addr, reg, bit;
  if      (port >= 1  && port <= 4)  { addr=NANO_MCP23_1_ADDR; reg=NANO_MCP23_OLATB; bit=port-1;  }
  else if (port >= 5  && port <= 8)  { addr=NANO_MCP23_1_ADDR; reg=NANO_MCP23_OLATA; bit=port-5;  }
  else if (port >= 9  && port <= 12) { addr=NANO_MCP23_2_ADDR; reg=NANO_MCP23_OLATB; bit=port-9;  }
  else return;
  uint8_t v = _mcpRead(addr, reg);
  _mcpWrite(addr, reg, on ? (v|(1<<bit)) : (v&~(1<<bit)));
}

// ── Public API ───────────────────────────────────────────────────────────
inline bool nanoSensorInit() {
  // Configure MCP23017 #1 (ports 1-8)
  _mcpWrite(NANO_MCP23_1_ADDR, NANO_MCP23_IODIRA, 0xF0);
  _mcpWrite(NANO_MCP23_1_ADDR, NANO_MCP23_IODIRB, 0xF0);
  _mcpWrite(NANO_MCP23_1_ADDR, NANO_MCP23_GPPUA,  0x00);
  _mcpWrite(NANO_MCP23_1_ADDR, NANO_MCP23_GPPUB,  0x00);
  _mcpWrite(NANO_MCP23_1_ADDR, NANO_MCP23_OLATA,  0x00);
  _mcpWrite(NANO_MCP23_1_ADDR, NANO_MCP23_OLATB,  0x00);

  // Configure MCP23017 #2 (ports 9-12)
  _mcpWrite(NANO_MCP23_2_ADDR, NANO_MCP23_IODIRA, 0xF0);
  _mcpWrite(NANO_MCP23_2_ADDR, NANO_MCP23_IODIRB, 0xF0);
  _mcpWrite(NANO_MCP23_2_ADDR, NANO_MCP23_GPPUA,  0x00);
  _mcpWrite(NANO_MCP23_2_ADDR, NANO_MCP23_GPPUB,  0x00);
  _mcpWrite(NANO_MCP23_2_ADDR, NANO_MCP23_OLATA,  0x00);
  _mcpWrite(NANO_MCP23_2_ADDR, NANO_MCP23_OLATB,  0x00);

  // Power on configured ports
  Serial.printf("[SENSOR] Powering %d port(s)\n", PORT_CONFIG_COUNT);
  for (int i = 0; i < PORT_CONFIG_COUNT; i++) {
    _sensorPower(PORT_CONFIG[i].port, true);
    Serial.printf("[SENSOR]   Port %2d ON  → %s addr=%d instance=%s\n",
                  PORT_CONFIG[i].port,
                  SENSOR_TYPES[PORT_CONFIG[i].type].type_name,
                  PORT_CONFIG[i].address,
                  PORT_CONFIG[i].instance);
  }
  delay(500);    // settle

  return PORT_CONFIG_COUNT > 0;
}

// อ่าน sensor ทุกตัวที่ config ไว้, เก็บลง _nanoReadings[]
// เรียกได้ทั้งแบบ one-shot และแบบ loop (ทุก NANO_READ_INTERVAL_MS)
static uint32_t _nanoSensorLastMs = 0;

inline void nanoSensorReadAll() {
  // Use millis-based timestamp to avoid DS3231 I2C glitch on ESP32-S3
  // where unixtime() alternates between correct and wildly wrong values.
  // Nano RTC stores Bangkok time (compile time = local Bangkok, interpreted as UTC).
  // So _nanoBootUtcSec is already Bangkok time — do NOT add offset again.
  uint64_t ts_bkk_ms =
    (uint64_t)(_nanoBootUtcSec) * 1000ULL
    + (uint64_t)(millis() - _nanoBootMs);

  for (int i = 0; i < PORT_CONFIG_COUNT; i++) {
    const PortConfig& cfg = PORT_CONFIG[i];
    SensorData d = readSensorData(cfg.type, cfg.address);
    _nanoReadings[i].valid      = d.valid;
    _nanoReadings[i].ts_bkk_ms  = ts_bkk_ms;
    if (d.valid) {
      _nanoReadings[i].data = d;
      Serial.printf("[READ] Port %d (%s) | ", cfg.port, SENSOR_TYPES[cfg.type].type_name);
      for (uint8_t f = 0; f < d.field_count; f++) {
        Serial.printf("%s=%.2f ", d.fields[f].key, d.fields[f].val);
      }
      Serial.println();
    } else {
      Serial.printf("[READ] Port %d (%s addr=%d): NO RESPONSE\n",
                    cfg.port, SENSOR_TYPES[cfg.type].type_name, cfg.address);
    }
  }
}

// Rate-limited wrapper — call from main loop()
inline void nanoSensorReadLoop() {
  uint32_t now = millis();
  if (now - _nanoSensorLastMs < NANO_READ_INTERVAL_MS) return;
  _nanoSensorLastMs = now;
  nanoSensorReadAll();
}

inline const NanoLastReading* nanoLastReading(int idx) {
  if (idx < 0 || idx >= PORT_CONFIG_COUNT) return nullptr;
  return &_nanoReadings[idx];
}
