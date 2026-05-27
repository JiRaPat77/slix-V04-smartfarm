// ════════════════════════════════════════════════════════════════════════
// SENSOR CONFIGURATION — กำหนด Sensor ที่ใช้งานในแต่ละ Port
//
// วิธีใช้:
//   1. ลบ // ออกหน้า port ที่มี sensor เสียบจริง
//   2. ตรวจสอบ address ให้ตรงกับ sensor จริง (ดูจาก datasheet หรือ label)
//   3. Build + Upload
//
// Address เริ่มต้นของแต่ละชนิด (factory default):
//   Soil RK520       → 1
//   Wind RK120       → 1
//   Solar RK200      → 1
//   Rain RK400       → 50 (0x32)
//   Ultrasonic RK500 → 50 (0x32)
//   Air Temp RS-WS   → 1
//   Soil EC RK500-23 → 4
//   Soil pH RK500-22 → 3
//   Water Level RKL01→ 1
// ════════════════════════════════════════════════════════════════════════
#pragma once
#include "sensor_types.h"

// Device ID ของ Gateway นี้ (ใช้ใน ThingsBoard device key)
#ifndef DEVICE_PREFIX
  #define DEVICE_PREFIX "SLXA1260004"
#endif

struct PortConfig {
    uint8_t      port;       // Port บนบอร์ด (1-16)
    SensorTypeID type;       // ชนิด sensor (ดูจาก sensor_types.h)
    uint8_t      address;    // Modbus address ของ sensor
    const char*  instance;   // ชื่อ instance สำหรับ ThingsBoard เช่น "01", "02"
};

// ── กำหนด Sensor ตรงนี้ ──────────────────────────────────────────────────
static const PortConfig PORT_CONFIG[] = {
//  port   type              address   instance
    { 1,   ST_SOIL,          1,         "01"  },  // Soil Moisture+Temp RK520
    { 2,   ST_SOIL,          2,         "02"  },
    { 3,   ST_WIND,          26,        "01"  },  // Wind Speed+Dir RK120
    { 4,   ST_AIR_TEMP,      14,        "01"  },  // Air Temp+Humidity RS-WS
    { 5,   ST_RAINFALL,      50,        "01"  },  // Rain Tip Counter RK400
    //  { 6,   ST_SOLAR,      50,       "01"  },  // Rain Tip Counter RK400
    //  { 7,   ST_ULTRASONIC,    50,       "01"  },  // Ultrasonic Distance
    //  { 8,   ST_SOIL_EC,       4,        "01"  },  // Soil EC+Salinity RK500-23
    //  { 9,   ST_SOIL_PH,       3,        "01"  },  // Soil pH RK500-22
    //  { 10,  ST_LIQUID_LEVEL,  1,        "01"  },  // Water Level RKL01
    //  { 11,  ST_SOIL,          2,        "02"  },  // Soil sensor ตัวที่ 2 (address ต่างกัน!)
};

static const int PORT_CONFIG_COUNT =
    sizeof(PORT_CONFIG) / sizeof(PortConfig);

// ── Build ThingsBoard device key ─────────────────────────────────────────
// Format: "SLXA1260004_Soil_RK520-01"
inline void buildDeviceKey(const PortConfig& cfg, char* out, size_t maxLen) {
    snprintf(out, maxLen, "%s_%s_%s-%s",
             DEVICE_PREFIX,
             SENSOR_TYPES[cfg.type].display_name,
             SENSOR_TYPES[cfg.type].model,
             cfg.instance);
}
