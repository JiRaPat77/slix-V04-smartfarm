// Device Telemetry Builder — สำหรับ topic v1/devices/me/telemetry
//
// ส่ง:
//   - Port_01..12_Connected   (จาก SENSOR_CHECK input ของ MCP23017)
//   - Port_01..12_Power       (จาก SENSOR_OUT  output ของ MCP23017)
//   - ambient_temperature/humidity (DHT20 @ 0x38)
//   - Board_Temp              (ESP32-S3 internal temperature sensor)
#pragma once
#include <Arduino.h>
#include <Wire.h>
#include <ArduinoJson.h>
#include <RTClib.h>
#include <math.h>

extern RTC_DS3231 rtc;

// ── MCP23017 register layout (เพิ่ม GPIO regs ที่ main.cpp ยังไม่มี) ─────
#ifndef MCP23_GPIOA
  #define MCP23_GPIOA 0x12
#endif
#ifndef MCP23_GPIOB
  #define MCP23_GPIOB 0x13
#endif
#ifndef MCP23_OLATA
  #define MCP23_OLATA 0x14
#endif
#ifndef MCP23_OLATB
  #define MCP23_OLATB 0x15
#endif

#ifndef MCP23_1_ADDR
  #define MCP23_1_ADDR 0x27
#endif
#ifndef MCP23_2_ADDR
  #define MCP23_2_ADDR 0x20
#endif

#define DHT20_ADDR  0x38

// helpers ที่อยู่ใน main.cpp อยู่แล้ว — re-declare เป็น weak fallback
static uint8_t _devTelMcpRead(uint8_t addr, uint8_t reg) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.endTransmission();
  Wire.requestFrom((int)addr, 1);
  return Wire.available() ? Wire.read() : 0;
}

// ── DHT20 read (port from Def code) ───────────────────────────────────────
static bool _readDHT20(float* outTemp, float* outHum) {
  Wire.beginTransmission(DHT20_ADDR);
  Wire.write(0xAC); Wire.write(0x33); Wire.write(0x00);
  if (Wire.endTransmission() != 0) return false;
  delay(80);
  Wire.requestFrom((int)DHT20_ADDR, (int)7);
  if (Wire.available() < 7) return false;
  uint8_t d[7];
  for (int i = 0; i < 7; i++) d[i] = Wire.read();
  if (d[0] & 0x80) return false;  // busy bit set
  uint32_t rawH = ((uint32_t)d[1] << 12) | ((uint32_t)d[2] << 4) | (d[3] >> 4);
  uint32_t rawT = ((uint32_t)(d[3] & 0x0F) << 16) | ((uint32_t)d[4] << 8) | d[5];
  *outHum  = (float)rawH / 1048576.0f * 100.0f;
  *outTemp = (float)rawT / 1048576.0f * 200.0f - 50.0f;
  return true;
}

// ── Board CPU temperature (ESP32-S3 internal) ────────────────────────────
extern "C" {
  uint8_t temprature_sens_read();  // legacy API
}
static float _readBoardTemp() {
  // ESP32-S3 มี API: temperatureRead() — return Celsius (float)
  // ถ้า lib version เก่า ใช้ temprature_sens_read() (raw, °F)
  #if defined(ESP_ARDUINO_VERSION) && ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(2,0,7)
    float t = temperatureRead();
    if (t < -40.0f || t > 125.0f) return NAN;
    return t;
  #else
    uint8_t r = temprature_sens_read();
    if (r == 128) return NAN;
    float f = (float)r;
    return (f - 32.0f) / 1.8f;  // °F → °C
  #endif
}

// ── อ่าน sensor_out (lower nibble = output state) ─────────────────────────
// num: 1..16 → คืน 0/1
static uint8_t _readSensorOut(uint8_t num) {
  uint8_t addr, reg, bit;
  if      (num >= 1  && num <= 4)  { addr=MCP23_1_ADDR; reg=MCP23_OLATB; bit=num-1;  }
  else if (num >= 5  && num <= 8)  { addr=MCP23_1_ADDR; reg=MCP23_OLATA; bit=num-5;  }
  else if (num >= 9  && num <= 12) { addr=MCP23_2_ADDR; reg=MCP23_OLATB; bit=num-9;  }
  else if (num >= 13 && num <= 16) { addr=MCP23_2_ADDR; reg=MCP23_OLATA; bit=num-13; }
  else return 0;
  return (_devTelMcpRead(addr, reg) >> bit) & 1;
}

// ── อ่าน sensor_check (upper nibble = input state, bit 4..7) ─────────────
static uint8_t _readSensorCheck(uint8_t num) {
  uint8_t addr, reg, bit;
  if      (num >= 1  && num <= 4)  { addr=MCP23_1_ADDR; reg=MCP23_GPIOB; bit=4 + (num-1);  }
  else if (num >= 5  && num <= 8)  { addr=MCP23_1_ADDR; reg=MCP23_GPIOA; bit=4 + (num-5);  }
  else if (num >= 9  && num <= 12) { addr=MCP23_2_ADDR; reg=MCP23_GPIOB; bit=4 + (num-9);  }
  else if (num >= 13 && num <= 16) { addr=MCP23_2_ADDR; reg=MCP23_GPIOA; bit=4 + (num-13); }
  else return 0;
  return (_devTelMcpRead(addr, reg) >> bit) & 1;
}

// ── สร้าง JSON payload สำหรับ topic v1/devices/me/telemetry ──────────────
// ส่งทุก port (1..12) เพราะบอร์ดมี 12 sensor port ตามภาพ PCB
// Format: [{"ts": <ms>, "values": {...}}]
static char _devTelPayload[2048];

inline const char* deviceTelemetryBuild() {
  uint64_t ts_ms = (uint64_t)rtc.now().unixtime() * 1000ULL;

  DynamicJsonDocument doc(2048);
  JsonArray arr  = doc.to<JsonArray>();
  JsonObject msg = arr.createNestedObject();
  msg["ts"] = ts_ms;
  JsonObject v  = msg.createNestedObject("values");

  // Port_NN_Connected และ Port_NN_Power สำหรับ port 1..12
  char key[24];
  for (uint8_t p = 1; p <= 12; p++) {
    snprintf(key, sizeof(key), "Port_%02u_Connected", p);
    v[key] = _readSensorCheck(p) ? "ON" : "OFF";
    snprintf(key, sizeof(key), "Port_%02u_Power", p);
    v[key] = _readSensorOut(p) ? "ON" : "OFF";
  }

  // DHT20 ambient temp/hum (I2C 0x38)
  float t = NAN, h = NAN;
  if (_readDHT20(&t, &h)) {
    v["ambient_temperature"] = roundf(t * 10.0f) / 10.0f;
    v["ambient_humidity"]    = roundf(h * 10.0f) / 10.0f;
  }

  // ESP32-S3 internal CPU temperature
  float bt = _readBoardTemp();
  if (!isnan(bt)) v["Board_Temp"] = roundf(bt * 10.0f) / 10.0f;

  serializeJson(doc, _devTelPayload, sizeof(_devTelPayload));
  return _devTelPayload;
}
