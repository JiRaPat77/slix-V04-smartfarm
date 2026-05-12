// Sensor Type Definitions — ported from Python class_sensor/*.py
// All sensors use Modbus RTU / RS485, FC03 Read Holding Registers
//
// Address-change registers verified from Python source:
//   Soil    RK520   : FC06 reg 0x0200
//   Wind    RK120   : FC06 reg 0x0020
//   Solar   RK200   : non-standard (custom command, no auto-readdress)
//   Rain    RK400   : FC06 reg 0x0100
//   Ultra   RK500   : FC06 reg 0x0100
//   AirTemp RS-WS   : FC06 reg 0x07D0
//   Soil EC RK500-23: FC06 reg 0x0014
//   Soil pH RK500-22: FC06 reg 0x0014
//   Level   RKL01   : FC06 reg 0x0000, then FC06 reg 0x000F (2-step save)
#pragma once
#include <Arduino.h>
#include <string.h>
#include "modbus_rtu.h"

// ── Sensor Type IDs ───────────────────────────────────────────────────────
enum SensorTypeID : uint8_t {
  ST_SOIL         = 0,
  ST_WIND         = 1,
  ST_SOLAR        = 2,
  ST_RAINFALL     = 3,
  ST_ULTRASONIC   = 4,
  ST_AIR_TEMP     = 5,
  ST_SOIL_EC      = 6,
  ST_SOIL_PH      = 7,
  ST_LIQUID_LEVEL = 8,
  ST_COUNT        = 9,
  ST_UNKNOWN      = 0xFF
};

// ── Per-sensor protocol definition ───────────────────────────────────────
struct SensorTypeDef {
  const char* type_name;      // "soil"
  const char* display_name;   // "Soil"
  const char* model;          // "RK520"
  uint8_t     addr_default;   // factory default Modbus address
  uint8_t     addr_min;       // range min (for collision resolution)
  uint8_t     addr_max;       // range max
  uint8_t     buf_size;       // telemetry buffer depth
  uint8_t     reg_count;      // number of 16-bit registers to read (FC03 count)
  uint16_t    modbus_reg;     // starting read register
  uint16_t    addr_change_reg;// FC06 register for address change (0xFFFF=special)
  uint32_t    baudrate;
};

// 0xFFFF = non-standard address change (solar), 0xFFFE = 2-step (liquid level)
#define ADDR_REG_SPECIAL  0xFFFF
#define ADDR_REG_TWOSTEP  0xFFFE

static const SensorTypeDef SENSOR_TYPES[ST_COUNT] = {
  //              name     display  model      def  min  max  buf  cnt   reg    adr_reg   baud
  /* ST_SOIL    */{"soil",   "Soil",  "RK520",   1,   2,  13, 10,   2, 0x0000, 0x0200, 9600},
  /* ST_WIND    */{"wind",   "Wind",  "RK120",   1,  14,  25, 10,   2, 0x0000, 0x0020, 9600},
  /* ST_SOLAR   */{"solar",  "Solar", "RK200",   1,  79,  91, 10,   1, 0x0000, ADDR_REG_SPECIAL, 9600},
  /* ST_RAINFALL*/{"rainfall","Rain", "RK400",  50,  51,  78, 20,   1, 0x0000, 0x0100, 9600},
  /* ST_ULTRA   */{"ultra",  "Ultra", "RK500",  50,  26,  50, 10,   1, 0x0000, 0x0100, 9600},
  /* ST_AIR_TEMP*/{"air_temp","Air",  "RS-WS",   1,  38,  50, 10,   2, 0x0000, 0x07D0, 9600},
  /* ST_SOIL_EC */{"soil_ec","SoilEC","RK500EC", 4,  51,  62, 10,  10, 0x0000, 0x0014, 9600},
  /* ST_SOIL_PH */{"soil_ph","SoilPH","RK500PH", 3,  63,  66, 10,   6, 0x0000, 0x0014, 9600},
  /* ST_LIQUID  */{"liquid", "Level", "RKL01",   1,  92, 103, 10,   1, 0x0004, ADDR_REG_TWOSTEP, 9600},
};

// ── Sensor Reading Result ─────────────────────────────────────────────────
#define MAX_SENSOR_FIELDS 4
struct SensorData {
  bool valid;
  uint8_t field_count;
  struct { const char* key; float val; } fields[MAX_SENSOR_FIELDS];
};
static inline SensorData _invalidData() {
  SensorData d{}; d.valid = false; d.field_count = 0; return d;
}

// ── Type helpers ──────────────────────────────────────────────────────────
inline SensorTypeID sensorTypeFromName(const char* name) {
  for (int i = 0; i < ST_COUNT; i++)
    if (strcmp(SENSOR_TYPES[i].type_name, name) == 0) return (SensorTypeID)i;
  return ST_UNKNOWN;
}
inline bool sensorAddressInRange(SensorTypeID type, uint8_t addr) {
  if (type >= ST_COUNT) return false;
  return addr >= SENSOR_TYPES[type].addr_min && addr <= SENSOR_TYPES[type].addr_max;
}
inline const char* sensorFieldKey(SensorTypeID type, uint8_t idx) {
  static const char* k[ST_COUNT][MAX_SENSOR_FIELDS] = {
    {"soil_temperature","soil_moisture", nullptr,      nullptr},  // SOIL
    {"wind_speed",      "wind_direction",nullptr,      nullptr},  // WIND
    {"solar_radiation", nullptr,         nullptr,      nullptr},  // SOLAR
    {"rainfall",        nullptr,         nullptr,      nullptr},  // RAINFALL
    {"distance_cm",     nullptr,         nullptr,      nullptr},  // ULTRA
    {"air_temperature", "air_humidity",  nullptr,      nullptr},  // AIR_TEMP
    {"soil_ec",         "soil_salinity", nullptr,      nullptr},  // SOIL_EC
    {"soil_ph",         "soil_temp",     nullptr,      nullptr},  // SOIL_PH
    {"water_level_cm",  nullptr,         nullptr,      nullptr},  // LIQUID
  };
  if (type >= ST_COUNT || idx >= MAX_SENSOR_FIELDS) return "value";
  return k[type][idx] ? k[type][idx] : "value";
}

// ── ชื่อสั้น parameter สำหรับ ThingsBoard device key ──────────────────
// Format: SLXA1260004_{Type}_{ParamShort}_{Model}-{Instance}
// e.g.  : SLXA1260004_Soil_Temp_RK520-01
inline const char* sensorParamShort(SensorTypeID type, uint8_t idx) {
  static const char* p[ST_COUNT][MAX_SENSOR_FIELDS] = {
    {"Temp",  "Moist",  nullptr,  nullptr},  // SOIL
    {"Speed", "Dir",    nullptr,  nullptr},  // WIND
    {"Rad",   nullptr,  nullptr,  nullptr},  // SOLAR
    {"Rain",  nullptr,  nullptr,  nullptr},  // RAINFALL
    {"Dist",  nullptr,  nullptr,  nullptr},  // ULTRA
    {"Temp",  "Hum",    nullptr,  nullptr},  // AIR_TEMP
    {"EC",    "Sal",    nullptr,  nullptr},  // SOIL_EC
    {"pH",    "Temp",   nullptr,  nullptr},  // SOIL_PH
    {"Level", nullptr,  nullptr,  nullptr},  // LIQUID
  };
  if (type >= ST_COUNT || idx >= MAX_SENSOR_FIELDS) return "Val";
  return p[type][idx] ? p[type][idx] : "Val";
}

// จำนวน parameter ที่ sensor ชนิดนี้มี
inline uint8_t sensorFieldCount(SensorTypeID type) {
  uint8_t cnt = SENSOR_TYPES[type].reg_count;
  // EC/pH ใช้ reg_count มาก แต่ return แค่ field จริง
  if (type == ST_SOIL_EC) return 2;
  if (type == ST_SOIL_PH) return 2;
  return (cnt >= 2) ? 2 : 1;
}

// ── IEEE-754 big-endian float parser ─────────────────────────────────────
static inline float _btof(const uint8_t* b) {
  uint32_t raw = ((uint32_t)b[0]<<24)|((uint32_t)b[1]<<16)|
                 ((uint32_t)b[2]<<8 )|(uint32_t)b[3];
  float f; memcpy(&f, &raw, sizeof(f)); return f;
}

// ── Read sensor data via Modbus ───────────────────────────────────────────
inline SensorData readSensorData(SensorTypeID type, uint8_t addr) {
  if (type >= ST_COUNT) return _invalidData();
  const SensorTypeDef& t = SENSOR_TYPES[type];

  // Buffer sized for largest response: EC uses 10 regs = 20 data bytes
  uint8_t raw[24] = {};
  if (modbusRead(addr, t.modbus_reg, t.reg_count, raw, t.baudrate) < 0)
    return _invalidData();

  SensorData d{}; d.valid = true;

  switch (type) {
    // ── Soil Moisture + Temperature ───────────────────────────────────────
    // reg0: temperature (S16 ×10), reg1: moisture (U16 ×10)
    case ST_SOIL: {
      int16_t tr = (int16_t)(((uint16_t)raw[0]<<8)|raw[1]);
      uint16_t mr = ((uint16_t)raw[2]<<8)|raw[3];
      float temp  = (float)tr  / 10.0f;
      float moist = (float)mr  / 10.0f;
      if (temp < -30.0f || temp > 70.0f || moist < 0.0f || moist > 100.0f)
        return _invalidData();
      d.field_count = 2;
      d.fields[0] = {"soil_temperature", temp};
      d.fields[1] = {"soil_moisture",    moist};
      break;
    }
    // ── Wind Speed + Direction ────────────────────────────────────────────
    // reg0: speed (U16 ×10 m/s), reg1: direction (U16 0-360°)
    case ST_WIND: {
      uint16_t sr = ((uint16_t)raw[0]<<8)|raw[1];
      uint16_t dr = ((uint16_t)raw[2]<<8)|raw[3];
      float spd = (float)sr / 10.0f;
      float dir = (float)dr;
      if (spd < 0.0f || spd > 70.0f || dir < 0.0f || dir > 360.0f)
        return _invalidData();
      d.field_count = 2;
      d.fields[0] = {"wind_speed",     spd};
      d.fields[1] = {"wind_direction", dir};
      break;
    }
    // ── Solar Radiation ───────────────────────────────────────────────────
    // reg0: radiation (U16, W/m²)
    case ST_SOLAR: {
      uint16_t rr = ((uint16_t)raw[0]<<8)|raw[1];
      float rad = (float)rr;
      if (rad < 0.0f || rad > 2000.0f) return _invalidData();
      d.field_count = 1;
      d.fields[0] = {"solar_radiation", rad};
      break;
    }
    // ── Rainfall Tip Counter ──────────────────────────────────────────────
    // reg0: tip count (U16), rainfall_mm = count × 0.2
    case ST_RAINFALL: {
      uint16_t cnt = ((uint16_t)raw[0]<<8)|raw[1];
      if (cnt > 10000) return _invalidData();
      d.field_count = 1;
      d.fields[0] = {"rainfall", (float)cnt * 0.2f};
      break;
    }
    // ── Ultrasonic Distance ───────────────────────────────────────────────
    // reg0: distance (U16, cm)
    case ST_ULTRASONIC: {
      uint16_t dist = ((uint16_t)raw[0]<<8)|raw[1];
      if (dist > 10000) return _invalidData();
      d.field_count = 1;
      d.fields[0] = {"distance_cm", (float)dist};
      break;
    }
    // ── Air Temperature + Humidity ────────────────────────────────────────
    // IMPORTANT: Python shows humidity FIRST, temperature SECOND
    // reg0: humidity (U16 ×10 %), reg1: temperature (S16 ×10 °C)
    case ST_AIR_TEMP: {
      uint16_t hr = ((uint16_t)raw[0]<<8)|raw[1];
      int16_t  tr = (int16_t)(((uint16_t)raw[2]<<8)|raw[3]);
      float hum  = (float)hr / 10.0f;
      float temp = (float)tr / 10.0f;
      d.field_count = 2;
      d.fields[0] = {"air_temperature", temp};
      d.fields[1] = {"air_humidity",    hum};
      break;
    }
    // ── Soil EC + Salinity (IEEE-754 floats) ──────────────────────────────
    // 10 registers (20 bytes): EC(4B), param1(4B), param2(4B), param3(4B), Salinity(4B)
    case ST_SOIL_EC: {
      float ec  = _btof(raw);       // bytes 0-3
      // bytes 4-7: param1, 8-11: param2, 12-15: param3 (skip)
      float sal = _btof(raw + 16);  // bytes 16-19 = salinity PPM
      if (ec < 0.0f || ec > 2000.0f) return _invalidData();
      d.field_count = 2;
      d.fields[0] = {"soil_ec",       ec};
      d.fields[1] = {"soil_salinity", sal};
      break;
    }
    // ── Soil pH + Temperature (IEEE-754 floats) ───────────────────────────
    // 6 registers (12 bytes): pH(4B), param(4B), temperature(4B)
    case ST_SOIL_PH: {
      float ph   = _btof(raw);      // bytes 0-3
      // bytes 4-7: param (skip)
      float temp = _btof(raw + 8);  // bytes 8-11 = temperature °C
      if (ph < 0.0f || ph > 14.0f) return _invalidData();
      d.field_count = 2;
      d.fields[0] = {"soil_ph",   ph};
      d.fields[1] = {"soil_temp", temp};
      break;
    }
    // ── Water Level ───────────────────────────────────────────────────────
    // reg 0x0004: raw value (U16), water_level_cm = raw / 10
    case ST_LIQUID_LEVEL: {
      uint16_t rv = ((uint16_t)raw[0]<<8)|raw[1];
      float lvl = (float)rv / 10.0f;
      if (lvl < 0.0f || lvl > 500.0f) return _invalidData();
      d.field_count = 1;
      d.fields[0] = {"water_level_cm", lvl};
      break;
    }
    default: return _invalidData();
  }
  return d;
}

// ── Address change — per-sensor protocol ─────────────────────────────────
// Returns true if address was successfully changed.
// Solar (ADDR_REG_SPECIAL): skipped — must be done manually via hardware jumper
// RKL01 (ADDR_REG_TWOSTEP): 2-step: write to 0x0000, then save to 0x000F
inline bool sensorChangeAddress(SensorTypeID type, uint8_t oldAddr, uint8_t newAddr) {
  if (type >= ST_COUNT) return false;
  const SensorTypeDef& t = SENSOR_TYPES[type];
  uint16_t reg = t.addr_change_reg;

  if (reg == ADDR_REG_SPECIAL) {
    // Solar RK200: non-standard command format, cannot auto-readdress
    Serial.printf("[ADDR] Solar RK200: address change not supported, set jumper manually\n");
    return false;
  }

  if (reg == ADDR_REG_TWOSTEP) {
    // RKL01 (water level): step1 write new address to reg 0x0000
    bool ok = modbusWriteReg(oldAddr, 0x0000, newAddr, t.baudrate);
    if (!ok) return false;
    delay(200);
    // Step2: save to reg 0x000F (write 0x0000 to trigger save)
    ok = modbusWriteReg(newAddr, 0x000F, 0x0000, t.baudrate);
    if (ok) delay(500);
    return ok;
  }

  // Standard FC06: write new_address to addr_change_reg
  bool ok = modbusChangeAddress(oldAddr, newAddr, reg, t.baudrate);
  Serial.printf("[ADDR] %s: FC06 reg=0x%04X %d→%d %s\n",
                t.type_name, reg, oldAddr, newAddr, ok ? "OK" : "FAILED");
  return ok;
}
