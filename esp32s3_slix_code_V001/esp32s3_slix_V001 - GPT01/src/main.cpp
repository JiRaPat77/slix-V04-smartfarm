// ════════════════════════════════════════════════════════════════════════
// ESP32-S3 Smart Farm Gateway — SLXA1260004
// Test Firmware (ไม่มี FreeRTOS — test ทีละ step)
//
// STEP 1 : MCP23017 — เปิดไฟ sensor port ที่กำหนด (bypass OC)
// STEP 2 : RS485     — อ่านค่า sensor ผ่าน Modbus RTU
// STEP 3 : ThingsBoard — ส่งข้อมูลขึ้น MQTT (per-parameter device, UTC+7)
//
// แก้ไขใน sensor_config.h เพื่อเพิ่ม/ลด sensor
// ════════════════════════════════════════════════════════════════════════

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <WiFi.h>
#include <Ethernet.h>
#include <EthernetUdp.h>
#include <WebServer.h>
#include <Preferences.h>
#include <RTClib.h>
#include <LittleFS.h>
#include <Adafruit_NeoPixel.h>
#include <ArduinoJson.h>
#include <time.h>
#include "eth_config.h"
#include "captive_portal.h"
#include "lan_webconfig.h"
#include "modbus_rtu.h"
#include "sensor_types.h"
#include "sensor_config.h"
#include "mqtt_tb.h"
#include "sd_card.h"
#include "offline_log.h"
#include "lora_protocol.h"
#include "lora_registry.h"
#include "lora_gateway.h"
#include "status_display.h"
#include "buttons.h"

// ESP32-S3 internal temperature sensor
extern "C" {
  uint8_t temprature_sens_read();   // legacy ESP32 API symbol (S3 has temperatureRead())
}

// ── Pins ─────────────────────────────────────────────────────────────────
#define NEOPIXEL_PIN   21
#define I2C_SDA        42
#define I2C_SCL        41
#define BTN_STATUS_PIN  45
#define BTN_PAIRING_PIN 46

// ── MCP23017 ─────────────────────────────────────────────────────────────
#define MCP23_1_ADDR  0x27
#define MCP23_2_ADDR  0x20
#define MCP23_3_ADDR  0x21
#define MCP23_IODIRA  0x00
#define MCP23_IODIRB  0x01
#define MCP23_GPPUA   0x0C
#define MCP23_GPPUB   0x0D
#define MCP23_GPIOA   0x12
#define MCP23_GPIOB   0x13
#define MCP23_OLATA   0x14
#define MCP23_OLATB   0x15

// ── DS3231 ────────────────────────────────────────────────────────────────
#define DS3231_ADDR  0x68
#define REG_CTRL     0x0E
#define REG_STATUS   0x0F

// ── DHT20 (I2C ambient temp/humidity) ─────────────────────────────────────
#define DHT20_ADDR   0x38

// ── Timezone ──────────────────────────────────────────────────────────────
// RTC เก็บเวลา UTC (sync จาก NTP)
// ThingsBoard ต้องการ UTC timestamp → ส่ง rtc.now().unixtime() * 1000 เลย
// ThingsBoard UI จะแปลงเป็นเวลาท้องถิ่นเอง (ตาม browser timezone)
#define BANGKOK_OFFSET_SEC  (7L * 3600L)
#define TZ_OFFSET_SEC       BANGKOK_OFFSET_SEC   // backward-compat alias

// ── Timing ───────────────────────────────────────────────────────────────
// แก้ค่าเหล่านี้ที่นี่ที่เดียว แล้ว rebuild
#define READ_INTERVAL_MS    5000    // อ่าน sensor ทุก 5 วิ
#define SEND_INTERVAL_MS    60000   // ส่ง ThingsBoard ทุก 60 วิ (ทั้ง gateway + device topic)
#define NET_CHECK_MS        10000   // เช็คเน็ตทุก 10 วิ
#define NTP_RESYNC_MS       (6L * 3600L * 1000L)  // re-sync NTP ทุก 6 ชม

// ── Globals ───────────────────────────────────────────────────────────────
Adafruit_NeoPixel pixels(1, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);
RTC_DS3231 rtc;
SPIClass eth_spi(FSPI);
NetworkConfig netConfig;
bool ap_mode_active    = false;
bool portal_active     = false;
bool lan_server_active = false;
bool wifi_connected    = false;
bool lan_connected     = false;
bool internet_connected = false;
bool internet_connecting = false;
WebServer portalServer(80);
WebServer lanServer(80);
WebServer logServer(8080);

// ── Millis-based UTC ──────────────────────────────────────────────────────
// Read RTC once at boot, then use millis() for all subsequent timestamps.
// This avoids the DS3231 I2C glitch where unixtime() alternates
// between correct and wildly wrong values on ESP32-S3.
uint32_t _bootUtcSec = 0;   // UTC seconds at boot (from RTC or NTP)
uint32_t _bootMs     = 0;   // millis() at the moment we captured _bootUtcSec

// Read RTC multiple times and take median to filter DS3231 I2C glitch
// (on ESP32-S3, rtc.now().unixtime() can alternate between correct and wildly wrong)
static void utcSyncFromRTC() {
  const int N = 7;
  uint32_t samples[N];
  for (int i = 0; i < N; i++) {
    samples[i] = rtc.now().unixtime();
    delay(20);
  }
  // Insertion sort
  for (int i = 1; i < N; i++) {
    uint32_t key = samples[i];
    int j = i - 1;
    while (j >= 0 && samples[j] > key) { samples[j+1] = samples[j]; j--; }
    samples[j+1] = key;
  }
  _bootUtcSec = samples[N/2];   // median — robust against outliers
  _bootMs     = millis();
  Serial.printf("[UTC] Synced from RTC (median of %d): bootUTC=%u\n", N, _bootUtcSec);
  for (int i = 0; i < N; i++) Serial.printf("  sample[%d]=%u\n", i, samples[i]);
}

// Set millis-based UTC directly from a known-good time source (NTP)
static void utcSyncFromValue(uint32_t utcSec) {
  _bootUtcSec = utcSec;
  _bootMs     = millis();
  Serial.printf("[UTC] Synced from NTP: bootUTC=%u\n", _bootUtcSec);
}

// Returns current UTC milliseconds (never glitches — pure arithmetic from boot reference)
static uint64_t utcNowMs() {
  return ((uint64_t)(_bootUtcSec) * 1000ULL) + ((uint64_t)(millis() - _bootMs));
}

// ── Median Buffer ─────────────────────────────────────────────────────────
// Circular buffer of N float readings per sensor field.
// median() returns the middle value after sorting — robust against outliers.
#define MEDIAN_BUF_SIZE  10

struct MedianBuf {
  float  buf[MEDIAN_BUF_SIZE];
  uint8_t count;   // how many slots filled (0..MEDIAN_BUF_SIZE)
  uint8_t idx;     // next write position (wraps around)

  MedianBuf() : count(0), idx(0) {
    for (int i = 0; i < MEDIAN_BUF_SIZE; i++) buf[i] = NAN;
  }

  void push(float v) {
    buf[idx] = v;
    idx = (idx + 1) % MEDIAN_BUF_SIZE;
    if (count < MEDIAN_BUF_SIZE) count++;
  }

  float median() const {
    if (count == 0) return NAN;
    float tmp[MEDIAN_BUF_SIZE];
    memcpy(tmp, buf, sizeof(float) * MEDIAN_BUF_SIZE);
    // insertion sort (only count elements)
    for (int i = 1; i < count; i++) {
      float key = tmp[i];
      int j = i - 1;
      while (j >= 0 && tmp[j] > key) { tmp[j+1] = tmp[j]; j--; }
      tmp[j+1] = key;
    }
    return tmp[count / 2];
  }
};

// Per-sensor-field median buffer (indexed by PORT_CONFIG index, then field)
struct SensorBuffer {
  bool       valid;
  MedianBuf  fields[MAX_SENSOR_FIELDS];
  SensorBuffer() : valid(false) {}
};
static SensorBuffer _sensorBuf[16] = {};   // one per PORT_CONFIG entry

// ════════════════════════════════════════════════════════════════════════
// SD logging helper — write to /log/YYYYMMDD.log
// ════════════════════════════════════════════════════════════════════════
static char _logBuf[256];

// static void logToSD(const char* line) {
//   if (!sdAvailable()) return;
//   DateTime now = rtc.now();
//   char path[40];
//   snprintf(path, sizeof(path), "/log/%04d%02d%02d.log",
//            now.year(), now.month(), now.day());
//   File f = SD.open(path, FILE_APPEND);
//   if (!f) return;
//   f.printf("[%02d:%02d:%02dZ] %s\n",
//            now.hour(), now.minute(), now.second(), line);
//   f.close();
// }

// // LOG: print to Serial AND mirror to SD log file
// #define LOG(fmt, ...) do { \
//   snprintf(_logBuf, sizeof(_logBuf), fmt, ##__VA_ARGS__); \
//   Serial.println(_logBuf); \
//   logToSD(_logBuf); \
// } while (0)

static void logToFS(const char* line) {
  // Use millis-based UTC to avoid DS3231 I2C glitch
  uint32_t utcSec = _bootUtcSec + (uint32_t)((millis() - _bootMs) / 1000UL);
  DateTime now(utcSec);
  char path[40];
  snprintf(path, sizeof(path), "/log/%04d%02d%02d.log",
           now.year(), now.month(), now.day());
           
  // Use LittleFS instead of SD. "a" stands for append mode.
  File f = LittleFS.open(path, "a");
  if (!f) return;
  f.printf("[%02d:%02d:%02dZ] %s\n",
           now.hour(), now.minute(), now.second(), line);
  f.close();
}

// LOG: print to Serial AND mirror to LittleFS log file
#define LOG(fmt, ...) do { \
  snprintf(_logBuf, sizeof(_logBuf), fmt, ##__VA_ARGS__); \
  Serial.println(_logBuf); \
  logToFS(_logBuf); \
} while (0)

// ════════════════════════════════════════════════════════════════════════
// MCP23017 helpers
// ════════════════════════════════════════════════════════════════════════
static void mcp_write(uint8_t addr, uint8_t reg, uint8_t val) {
  Wire.beginTransmission(addr);
  Wire.write(reg); Wire.write(val);
  Wire.endTransmission();
}
static uint8_t mcp_read(uint8_t addr, uint8_t reg) {
  Wire.beginTransmission(addr); Wire.write(reg); Wire.endTransmission();
  Wire.requestFrom((int)addr, 1);
  return Wire.read();
}

// เปิด/ปิดไฟเลี้ยง sensor แต่ละ port (ผ่าน MCP23017 #1, #2)
void sensor_en_set(uint8_t num, bool on) {
  uint8_t addr, reg, bit;
  if      (num >= 1  && num <= 4)  { addr=MCP23_1_ADDR; reg=MCP23_OLATB; bit=num-1;  }
  else if (num >= 5  && num <= 8)  { addr=MCP23_1_ADDR; reg=MCP23_OLATA; bit=num-5;  }
  else if (num >= 9  && num <= 12) { addr=MCP23_2_ADDR; reg=MCP23_OLATB; bit=num-9;  }
  else if (num >= 13 && num <= 16) { addr=MCP23_2_ADDR; reg=MCP23_OLATA; bit=num-13; }
  else return;
  uint8_t v = mcp_read(addr, reg);
  mcp_write(addr, reg, on ? (v|(1<<bit)) : (v&~(1<<bit)));
}

// ════════════════════════════════════════════════════════════════════════
// STEP 1: Hardware init + Power ON sensor ports
// ════════════════════════════════════════════════════════════════════════
static void step1_init() {
  Serial.println("\n[STEP1] MCP23017 Power Control");

  // MCP23017 #1 — ports 1-8 (lower nibble = output, upper nibble = OC input)
  mcp_write(MCP23_1_ADDR, MCP23_IODIRA, 0xF0);
  mcp_write(MCP23_1_ADDR, MCP23_IODIRB, 0xF0);
  mcp_write(MCP23_1_ADDR, MCP23_GPPUA,  0x00);
  mcp_write(MCP23_1_ADDR, MCP23_GPPUB,  0x00);
  mcp_write(MCP23_1_ADDR, MCP23_OLATA,  0x00);
  mcp_write(MCP23_1_ADDR, MCP23_OLATB,  0x00);
  Serial.println("[STEP1] MCP23017 #1 (0x27) OK");

  // MCP23017 #2 — ports 9-16
  mcp_write(MCP23_2_ADDR, MCP23_IODIRA, 0xF0);
  mcp_write(MCP23_2_ADDR, MCP23_IODIRB, 0xF0);
  mcp_write(MCP23_2_ADDR, MCP23_GPPUA,  0x00);
  mcp_write(MCP23_2_ADDR, MCP23_GPPUB,  0x00);
  mcp_write(MCP23_2_ADDR, MCP23_OLATA,  0x00);
  mcp_write(MCP23_2_ADDR, MCP23_OLATB,  0x00);
  Serial.println("[STEP1] MCP23017 #2 (0x20) OK");

  // เปิดไฟเฉพาะ port ที่กำหนดใน sensor_config.h (bypass OC)
  Serial.printf("[STEP1] Powering %d port(s)...\n", PORT_CONFIG_COUNT);
  for (int i = 0; i < PORT_CONFIG_COUNT; i++) {
    sensor_en_set(PORT_CONFIG[i].port, true);
    Serial.printf("[STEP1]   Port %2d ON  → %s addr=%d\n",
                  PORT_CONFIG[i].port,
                  SENSOR_TYPES[PORT_CONFIG[i].type].type_name,
                  PORT_CONFIG[i].address);
  }
  delay(500); // รอไฟเสถียร
  Serial.println("[STEP1] DONE ✓");
}

// ════════════════════════════════════════════════════════════════════════
// STEP 2: Read sensors via RS485 Modbus RTU
// ════════════════════════════════════════════════════════════════════════

// เก็บ last reading ของแต่ละ port (สำหรับ send ทุก 60s)
struct LastReading {
  bool        valid;
  SensorData  data;
  uint32_t    ts_bangkok; // unix timestamp (Bangkok time)
};
static LastReading lastReadings[16] = {}; // index = PORT_CONFIG index

static void step2_read() {
  Serial.println("[STEP2] Reading sensors...");

  for (int i = 0; i < PORT_CONFIG_COUNT; i++) {
    const PortConfig& cfg = PORT_CONFIG[i];
    SensorData data = readSensorData(cfg.type, cfg.address);

    if (data.valid) {
      lastReadings[i].valid = true;
      lastReadings[i].data   = data;

      // Push each field into median buffer
      _sensorBuf[i].valid = true;
      for (uint8_t f = 0; f < data.field_count; f++) {
        _sensorBuf[i].fields[f].push(data.fields[f].val);
      }

      Serial.printf("[STEP2]   Port %d | %s | ",
                    cfg.port, SENSOR_TYPES[cfg.type].type_name);
      for (uint8_t f = 0; f < data.field_count; f++) {
        Serial.printf("%s=%.2f  ", data.fields[f].key, data.fields[f].val);
      }
      Serial.println();
    } else {
      lastReadings[i].valid = false;
      Serial.printf("[STEP2]   Port %d | %s addr=%d : NO RESPONSE\n",
                    cfg.port,
                    SENSOR_TYPES[cfg.type].type_name,
                    cfg.address);
    }
  }
  Serial.flush();
}

// ════════════════════════════════════════════════════════════════════════
// STEP 3: Build ThingsBoard payload + send via MQTT
//
// Format: แต่ละ parameter = device แยก
//   SLXA1260004_Soil_Temp_RK520-01  → current_status, operation_status, data_value
//   SLXA1260004_Soil_Moist_RK520-01 → current_status, operation_status, data_value
// Timezone: Bangkok (UTC+7) → ส่ง UTC ไป ThingsBoard
// ════════════════════════════════════════════════════════════════════════
static char _payload[8192];

// ts_ms = timestamp (Bangkok unix milliseconds) ที่ใช้ร่วมกันทุก device ในรอบส่งนี้
static const char* step3_build_payload(uint64_t ts_ms) {
  DynamicJsonDocument doc(8192);
  bool hasData = false;

  // ── Wired sensors (PORT_CONFIG) ──────────────────────────────────────
  for (int i = 0; i < PORT_CONFIG_COUNT; i++) {
    const PortConfig& cfg   = PORT_CONFIG[i];
    const LastReading& lr   = lastReadings[i];
    const SensorTypeDef& td = SENSOR_TYPES[cfg.type];

    const char* op_status  = lr.valid ? "online"  : "offline";
    const char* cur_status = lr.valid ? "healthy" : "weekly";
    uint8_t nFields = lr.valid ? lr.data.field_count : sensorFieldCount(cfg.type);

    for (uint8_t f = 0; f < nFields; f++) {
      char devKey[64];
      snprintf(devKey, sizeof(devKey), "%s_%s_%s_%s-%s",
               DEVICE_PREFIX,
               td.display_name,
               sensorParamShort(cfg.type, f),
               td.model,
               cfg.instance);

      JsonArray arr = doc.createNestedArray(devKey);
      JsonObject msg = arr.createNestedObject();
      msg["ts"] = ts_ms;          // ts ร่วมของรอบส่งนี้
      JsonObject vals = msg.createNestedObject("values");
      vals["current_status"]   = cur_status;
      vals["operation_status"] = op_status;
      if (lr.valid && f < lr.data.field_count) {
        // Use median value if buffer has data, otherwise use raw reading
        float val = (_sensorBuf[i].valid && _sensorBuf[i].fields[f].count > 0)
                    ? _sensorBuf[i].fields[f].median()
                    : lr.data.fields[f].val;
        vals["data_value"] = roundf(val * 100.0f) / 100.0f;
      }
      hasData = true;
    }
  }

  // ── LoRa virtual sensors — same device key format as local sensors ───────
  // Device key: SLXA1260004_{Type}_{Param}_{Model}-{Instance}
  // e.g. local:  SLXA1260004_Soil_Moist_RK520-01  ..05
  //      LoRa:   SLXA1260004_Soil_Moist_RK520-06  (continues from local)
  //
  // Freshness: use millis()-based check (last_rx_ms) instead of comparing
  // timestamps, so we don't depend on nano RTC accuracy.

  // Find max instance per sensor type from local PORT_CONFIG
  uint8_t _loraNextInst[ST_COUNT] = {};
  for (int i = 0; i < PORT_CONFIG_COUNT; i++) {
    uint8_t inst = (uint8_t)atoi(PORT_CONFIG[i].instance);
    SensorTypeID t = PORT_CONFIG[i].type;
    if (inst >= _loraNextInst[t]) _loraNextInst[t] = inst;
  }
  // _loraNextInst[t] now = highest local instance for type t

  for (int i = 0; i < LORA_MAX_NODES; i++) {
    LoRaNode* nd = loraRegistrySlot(i);
    if (!nd) continue;

    for (int s = 0; s < nd->sensor_count; s++) {
      LoRaNodeSensor& sensor = nd->sensors[s];
      if (!sensor.used) continue;

      const SensorTypeDef& td = SENSOR_TYPES[sensor.type];
      // Freshness: heard from this node within last 2 minutes (millis-based)
      bool fresh = sensor.data_valid && nd->last_rx_ms > 0 &&
                   (millis() - nd->last_rx_ms) < 120000UL;
      const char* op_status  = fresh ? "online"  : "offline";
      const char* cur_status = fresh ? "healthy" : "weekly";
      uint8_t nFields = fresh ? sensor.field_count : sensorFieldCount(sensor.type);

      // Instance: Fixed mapping (4 slots per Nano per sensor type)
      // nodeNum = 1 for "N1", 2 for "N2", etc.
      int nodeNum = (nd->id[0] == 'N') ? atoi(nd->id + 1) : 0;
      if (nodeNum < 1) nodeNum = 1; // fallback
      
      uint8_t localMax = _loraNextInst[sensor.type];
      uint8_t nanoInst = (uint8_t)atoi(sensor.instance); // "01" -> 1
      uint8_t finalInst = localMax + ((nodeNum - 1) * 4) + nanoInst;

      char instance[4];
      snprintf(instance, sizeof(instance), "%02d", finalInst);

      for (uint8_t f = 0; f < nFields; f++) {
        char devKey[64];
        snprintf(devKey, sizeof(devKey), "%s_%s_%s_%s-%s",
                 DEVICE_PREFIX,
                 td.display_name,
                 sensorParamShort(sensor.type, f),
                 td.model,
                 instance);

        JsonArray arr = doc.createNestedArray(devKey);
        JsonObject msg = arr.createNestedObject();
        msg["ts"] = ts_ms;          // same gateway timestamp as local sensors
        JsonObject vals = msg.createNestedObject("values");
        vals["current_status"]   = cur_status;
        vals["operation_status"] = op_status;
        if (fresh && f < sensor.field_count) {
          vals["data_value"] = roundf(sensor.fields[f] * 100.0f) / 100.0f;
        }
        hasData = true;
      }
    }
  }

  if (!hasData) return nullptr;
  serializeJson(doc, _payload, sizeof(_payload));
  return _payload;
}

static void step3_send(uint64_t ts_ms) {
  Serial.println("[STEP3] Building ThingsBoard payload...");
  const char* payload = step3_build_payload(ts_ms);
  if (!payload) {
    Serial.println("[STEP3] No data to send");
    return;
  }

  Serial.printf("[STEP3] Payload (%d bytes): %s\n",
                strlen(payload), payload);
  Serial.flush();

  bool sent = false;
  if (internet_connected && mqttIsConnected()) {
    sent = mqttPublish(payload);
  }

  if (sent) {
    LOG("[STEP3] Sent OK (live)");
    pixels.setPixelColor(0, pixels.Color(0, 255, 0)); pixels.show();
    delay(150);
  } else {
    // Offline หรือ MQTT fail → เก็บลง queue พร้อม timestamp ตอนอ่าน
    if (offlineEnqueue(payload)) {
      LOG("[STEP3] Offline → queued (pending=%u)", offlinePendingCount());
      pixels.setPixelColor(0, pixels.Color(255, 165, 0)); pixels.show();
    } else {
      LOG("[STEP3] Offline enqueue FAILED");
      pixels.setPixelColor(0, pixels.Color(255, 0, 0)); pixels.show();
    }
    delay(150);
  }
}

// ════════════════════════════════════════════════════════════════════════
// DHT20 (I2C) — อุณหภูมิ + ความชื้นของบอร์ด
// ════════════════════════════════════════════════════════════════════════
static bool readDHT20(float* temp, float* hum) {
  Wire.beginTransmission(DHT20_ADDR);
  Wire.write(0xAC); Wire.write(0x33); Wire.write(0x00);
  if (Wire.endTransmission() != 0) return false;
  delay(80);
  Wire.requestFrom((int)DHT20_ADDR, (int)7);
  if (Wire.available() < 7) return false;
  uint8_t d[7];
  for (int i = 0; i < 7; i++) d[i] = Wire.read();
  if (d[0] & 0x80) return false;  // sensor busy
  uint32_t rawH = ((uint32_t)d[1] << 12) | ((uint32_t)d[2] << 4) | (d[3] >> 4);
  uint32_t rawT = ((uint32_t)(d[3] & 0x0F) << 16) | ((uint32_t)d[4] << 8) | d[5];
  *hum  = (float)rawH / 1048576.0f * 100.0f;
  *temp = (float)rawT / 1048576.0f * 200.0f - 50.0f;
  return true;
}

// ════════════════════════════════════════════════════════════════════════
// Board CPU temperature (ESP32-S3 internal sensor)
// คืนค่า NAN ถ้าอ่านไม่ได้
// ════════════════════════════════════════════════════════════════════════
static float readBoardTemp() {
#if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(ARDUINO_ARCH_ESP32)
  // Arduino-ESP32 v2.0.5+ provides temperatureRead() on S3
  float t = temperatureRead();
  if (isnan(t) || t < -40.0f || t > 125.0f) return NAN;
  return t;
#else
  return NAN;
#endif
}

// ════════════════════════════════════════════════════════════════════════
// อ่านสถานะ SENSOR_OUT (lower nibble OLAT) + SENSOR_CHECK (upper nibble GPIO)
// out_state[0..11], chk_state[0..11] (port 1..12)
// ════════════════════════════════════════════════════════════════════════
static void readPortStates(bool out_state[12], bool chk_state[12]) {
  uint8_t olat_b1 = mcp_read(MCP23_1_ADDR, MCP23_OLATB);  // ports 1-4 out
  uint8_t olat_a1 = mcp_read(MCP23_1_ADDR, MCP23_OLATA);  // ports 5-8 out
  uint8_t olat_b2 = mcp_read(MCP23_2_ADDR, MCP23_OLATB);  // ports 9-12 out
  uint8_t gpio_b1 = mcp_read(MCP23_1_ADDR, MCP23_GPIOB);  // ports 1-4 chk (upper)
  uint8_t gpio_a1 = mcp_read(MCP23_1_ADDR, MCP23_GPIOA);  // ports 5-8 chk (upper)
  uint8_t gpio_b2 = mcp_read(MCP23_2_ADDR, MCP23_GPIOB);  // ports 9-12 chk (upper)

  for (int i = 0; i < 4; i++) {
    out_state[i]      = (olat_b1 >> i) & 1;            // port 1-4
    out_state[4 + i]  = (olat_a1 >> i) & 1;            // port 5-8
    out_state[8 + i]  = (olat_b2 >> i) & 1;            // port 9-12
    chk_state[i]      = (gpio_b1 >> (4 + i)) & 1;
    chk_state[4 + i]  = (gpio_a1 >> (4 + i)) & 1;
    chk_state[8 + i]  = (gpio_b2 >> (4 + i)) & 1;
  }
}

// ════════════════════════════════════════════════════════════════════════
// STEP 4: Device telemetry → v1/devices/me/telemetry
// ส่ง: Port_NN_Connected/Power, ambient_temperature/humidity, Board_Temp
// ════════════════════════════════════════════════════════════════════════
static char _devTelPayload[2048];

static void step4_send_device_telemetry(uint64_t ts_ms) {
  Serial.println("[STEP4] Building device telemetry...");

  bool out_st[12], chk_st[12];
  readPortStates(out_st, chk_st);

  float amb_t = NAN, amb_h = NAN;
  bool dht_ok = readDHT20(&amb_t, &amb_h);

  float brd_t = readBoardTemp();

  DynamicJsonDocument doc(2048);
  // ThingsBoard device telemetry รับได้ทั้ง array หรือ object — ใช้ array มี ts
  JsonArray arr = doc.to<JsonArray>();
  JsonObject msg = arr.createNestedObject();
  msg["ts"] = ts_ms;
  JsonObject v = msg.createNestedObject("values");

  char k[24];
  for (int i = 0; i < 12; i++) {
    snprintf(k, sizeof(k), "Port_%02d_Connected", i + 1);
    v[k] = chk_st[i] ? "ON" : "OFF";
    snprintf(k, sizeof(k), "Port_%02d_Power", i + 1);
    v[k] = out_st[i] ? "ON" : "OFF";
  }

  if (dht_ok) {
    v["ambient_temperature"] = roundf(amb_t * 10.0f) / 10.0f;
    v["ambient_humidity"]    = roundf(amb_h * 10.0f) / 10.0f;
  }
  if (!isnan(brd_t)) {
    v["Board_Temp"] = roundf(brd_t * 10.0f) / 10.0f;
  }

  size_t n = serializeJson(doc, _devTelPayload, sizeof(_devTelPayload));
  Serial.printf("[STEP4] Payload (%u bytes): %.180s%s\n",
                (unsigned)n, _devTelPayload, n > 180 ? "..." : "");

  if (internet_connected && mqttIsConnected()) {
    if (mqttPublishDevice(_devTelPayload, n)) {
      LOG("[STEP4] Device telemetry sent OK");
    } else {
      LOG("[STEP4] Device telemetry send FAILED");
    }
  } else {
    LOG("[STEP4] Skipped (no MQTT/internet)");
  }
}

// ════════════════════════════════════════════════════════════════════════
// Replay queue ที่เก็บไว้ตอน offline → ยิงด้วย timestamp ของตอนอ่านจริง
// (payload ถูก build พร้อม "ts" ใน step3 อยู่แล้ว — แค่ publish ของเดิม)
// ════════════════════════════════════════════════════════════════════════
static char _replayBuf[8192];

static void replayOfflineQueue() {
  if (!offlineHasPending() || !mqttIsConnected()) return;

  int sentCount = 0;
  const int MAX_PER_CALL = 5;   // กัน blocking — ไล่ส่งทีละ batch
  while (offlineHasPending() && sentCount < MAX_PER_CALL) {
    if (!offlinePeek(_replayBuf, sizeof(_replayBuf))) break;
    if (mqttPublish(_replayBuf, strlen(_replayBuf))) {
      offlineAck();
      sentCount++;
      delay(100);  // pacing เล็กน้อย ไม่ให้ broker overwhelm
    } else {
      LOG("[REPLAY] Publish failed — stop, will retry next cycle");
      break;
    }
  }
  if (sentCount > 0) {
    LOG("[REPLAY] Sent %d batch, %u remaining", sentCount, offlinePendingCount());
  }
}

// ════════════════════════════════════════════════════════════════════════
// NTP Sync — set RTC to UTC
// WiFi: ใช้ configTime() (lwip native)
// LAN : ใช้ EthernetUDP yiging NTP packet ตรงๆ (W5500 bypass lwip)
// ════════════════════════════════════════════════════════════════════════
static bool      _ntpSynced     = false;
static uint32_t  _lastNtpSync   = 0;

static bool _ntp_via_wifi() {
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  struct tm ti = {};
  for (int i = 0; i < 20; i++) {
    if (getLocalTime(&ti, 500)) {
      // Build UTC unix seconds directly from NTP — never re-read glitchy RTC
      struct tm utc_tm = {};
      utc_tm.tm_year = ti.tm_year;
      utc_tm.tm_mon  = ti.tm_mon;
      utc_tm.tm_mday = ti.tm_mday;
      utc_tm.tm_hour = ti.tm_hour;
      utc_tm.tm_min  = ti.tm_min;
      utc_tm.tm_sec = ti.tm_sec;
      uint32_t ntpUtcSec = (uint32_t)mktime(&utc_tm);
      // Also write to RTC so it stays correct for next boot
      rtc.adjust(DateTime(ntpUtcSec));
      // Sync millis-based UTC directly from NTP value (not from RTC read)
      utcSyncFromValue(ntpUtcSec);
      LOG("[NTP] WiFi sync OK → UTC=%u", ntpUtcSec);
      return true;
    }
    delay(300);
  }
  return false;
}

static bool _ntp_via_lan() {
  EthernetUDP udp;
  if (!udp.begin(2390)) return false;

  uint8_t buf[48] = {};
  buf[0] = 0xE3; // LI=3, Version=4, Mode=3 (client)

  // ใช้ IP ตรงๆ กัน DNS เน่า (time.google.com NTP)
  IPAddress ntpIP(216, 239, 35, 0);
  if (!udp.beginPacket(ntpIP, 123)) { udp.stop(); return false; }
  udp.write(buf, 48);
  udp.endPacket();

  uint32_t t0 = millis();
  while (millis() - t0 < 2000) {
    if (udp.parsePacket() >= 48) {
      udp.read(buf, 48);
      uint32_t secs = ((uint32_t)buf[40] << 24) | ((uint32_t)buf[41] << 16) |
                      ((uint32_t)buf[42] << 8)  | (uint32_t)buf[43];
      if (secs > 2208988800UL) {
        secs -= 2208988800UL;  // NTP epoch (1900) → Unix epoch (1970)
        rtc.adjust(DateTime(secs));
        // Sync millis-based UTC directly from NTP value (not from glitchy RTC read)
        utcSyncFromValue(secs);
        LOG("[NTP] LAN sync OK → UTC=%u", secs);
        udp.stop();
        return true;
      }
    }
    delay(20);
  }
  udp.stop();
  return false;
}

static bool syncNTP() {
  Serial.print("[NTP] Syncing");
  bool ok = false;
  if (wifi_connected)      ok = _ntp_via_wifi();
  else if (lan_connected)  ok = _ntp_via_lan();
  if (!ok) Serial.println(" FAILED");
  return ok;
}

// ════════════════════════════════════════════════════════════════════════
// Network helpers
// ════════════════════════════════════════════════════════════════════════
static void initLAN() {
  Serial.println("[NET] Init W5500 (LAN)...");
  eth_spi.begin(ETH_SCLK, ETH_MISO, ETH_MOSI, ETH_CS);
  pinMode(ETH_RST, OUTPUT);
  digitalWrite(ETH_RST, LOW); delay(20);
  digitalWrite(ETH_RST, HIGH); delay(150);
  Ethernet.init(ETH_CS);
  byte mac[] = {0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED};
  if (netConfig.ip_mode == 1) {
    IPAddress ip, sn, gw, dns;
    ip.fromString(netConfig.static_ip); sn.fromString(netConfig.subnet);
    gw.fromString(netConfig.gateway);   dns.fromString(netConfig.dns);
    Ethernet.begin(mac, ip, dns, gw, sn);
  } else {
    if (Ethernet.begin(mac, 8000) == 0) {
      Serial.println("[NET] LAN DHCP FAILED");
      return;
    }
  }
  lan_connected = true; internet_connected = true;
  Serial.printf("[NET] LAN IP: %s\n", Ethernet.localIP().toString().c_str());
  startLANWebServer();
}

static void initWifi() {
  if (!netConfig.wifi_ssid[0]) { startCaptivePortal(); return; }
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);    // ESP32 จะพยายาม reconnect เองเมื่อ WiFi หลุด
  WiFi.persistent(true);
  WiFi.begin(netConfig.wifi_ssid, netConfig.wifi_pass);
  Serial.printf("[NET] WiFi → '%s'...", netConfig.wifi_ssid);
  for (int i = 0; i < 30 && WiFi.status() != WL_CONNECTED; i++) {
    delay(500); Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    wifi_connected = true; internet_connected = true;
    Serial.printf("[NET] WiFi IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("[NET] WiFi FAILED → Captive Portal");
    startCaptivePortal();
  }
}

static void checkNet() {
  if (lan_connected) Ethernet.maintain();

  // ── WiFi auto-reconnect ──────────────────────────────────────────────
  // ถ้าตั้ง SSID ไว้แล้ว แต่ disconnect → ไล่ reconnect ทุก 30 วิ
  if (netConfig.wifi_ssid[0]) {
    bool current = (WiFi.status() == WL_CONNECTED);
    if (!current) {
      static uint32_t _lastWifiRetry = 0;
      uint32_t now = millis();
      if (now - _lastWifiRetry > 30000) {
        _lastWifiRetry = now;
        Serial.println("[WIFI] Reconnecting...");
        WiFi.reconnect();
      }
    }
    wifi_connected = current;
  }
  internet_connected = (wifi_connected || lan_connected);

  if (internet_connected && !mqttIsConnected()) mqttConnect();
  if (mqttIsConnected()) mqttLoop();

  // NTP: sync ครั้งแรก + ทุก 6 ชม
  if (internet_connected) {
    uint32_t now = millis();
    if (!_ntpSynced || (now - _lastNtpSync) > NTP_RESYNC_MS) {
      if (syncNTP()) {
        _ntpSynced  = true;
        _lastNtpSync = now;
        // utcSyncFromValue() already called inside _ntp_via_wifi/_ntp_via_lan
        // No need to re-read glitchy RTC here
      }
    }
  }

  // ไล่ส่ง offline queue ทุกครั้งที่ MQTT ยังต่อได้ + มี pending
  if (mqttIsConnected()) replayOfflineQueue();
}

// ════════════════════════════════════════════════════════════════════════
// I2C bus recovery
// ════════════════════════════════════════════════════════════════════════
static void i2cBusRecovery() {
  pinMode(I2C_SCL, OUTPUT); pinMode(I2C_SDA, INPUT_PULLUP);
  for (int i = 0; i < 9; i++) {
    digitalWrite(I2C_SCL, HIGH); delayMicroseconds(5);
    digitalWrite(I2C_SCL, LOW);  delayMicroseconds(5);
  }
  pinMode(I2C_SDA, OUTPUT);
  digitalWrite(I2C_SDA, LOW);  delayMicroseconds(5);
  digitalWrite(I2C_SCL, HIGH); delayMicroseconds(5);
  digitalWrite(I2C_SDA, HIGH); delayMicroseconds(5);
  pinMode(I2C_SCL, INPUT_PULLUP);
  pinMode(I2C_SDA, INPUT_PULLUP);
}


// ════════════════════════════════════════════════════════════════════════
// Web Server view log port 8080
// ════════════════════════════════════════════════════════════════════════
void setupLogServer() {
  logServer.on("/logs", HTTP_GET, []() {
    String html = "<html><body style='font-family:sans-serif; background:#111827; color:#f3f4f6; padding:20px;'>";
    html += "<h2 style='color:#38bdf8;'>&#128220; System Logs (LittleFS)</h2><ul>";
    
    File dir = LittleFS.open("/log");
    if (!dir || !dir.isDirectory()) {
      html += "<li>No logs found.</li>";
    } else {
      File file = dir.openNextFile();
      while (file) {
        String fname = String(file.name());
        if (fname.endsWith(".log")) {
          html += "<li style='margin-bottom:12px; font-size:18px;'>";
          html += "<a style='color:#0ea5e9; text-decoration:none;' href='/read?file=" + fname + "'>" + fname + "</a> ";
          html += "<span style='color:#6b7280; font-size:14px;'>(" + String(file.size()) + " bytes)</span>";
          html += "</li>";
        }
        file = dir.openNextFile();
      }
    }
    html += "</ul></body></html>";
    logServer.send(200, "text/html", html);
  });

  logServer.on("/read", HTTP_GET, []() {
    if (logServer.hasArg("file")) {
      String path = "/log/" + logServer.arg("file");
      File f = LittleFS.open(path, "r");
      if (f) {
        logServer.streamFile(f, "text/plain");
        f.close();
        return;
      }
    }
    logServer.send(404, "text/plain", "404: File not found");
  });

  logServer.begin();
  Serial.println("[SYS]  Log Server started on port 8080");
}

// ════════════════════════════════════════════════════════════════════════
// Setup
// ════════════════════════════════════════════════════════════════════════
void setup() {
  pixels.begin(); pixels.setBrightness(40);
  pixels.setPixelColor(0, pixels.Color(255, 165, 0)); pixels.show(); // orange = booting

  Serial.begin(115200);
  delay(3000);
  Serial.println("\n=========================================");
  Serial.println("[SYS]  ESP32-S3 Smart Farm Gateway TEST");
  Serial.printf("[SYS]  Device: %s\n", DEVICE_PREFIX);
  Serial.println("=========================================\n");
  Serial.flush();

  // I2C
  i2cBusRecovery();
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setTimeOut(200);

  // Buttons (status / pairing)
  buttonsBegin();

  // RS485 init
  modbusInit();
  Serial.println("[SYS]  RS485 Serial1 GPIO43/44 OK");

  // ── STEP 1: MCP power ─────────────────────────────────────────────────
  step1_init();

  // RTC — RTC จะถูก sync เป็น UTC จาก NTP หลัง internet พร้อม
  if (rtc.begin(&Wire)) {
    if (rtc.lostPower()) rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    DateTime t = rtc.now();
    Serial.printf("[RTC]  %04d-%02d-%02d %02d:%02d:%02d (pre-NTP)\n",
                  t.year(), t.month(), t.day(), t.hour(), t.minute(), t.second());
    utcSyncFromRTC();   // Initialize millis-based UTC clock from RTC
  } else {
    Serial.println("[RTC]  DS3231 not found!");
    // Fallback: use compile time as rough UTC estimate
    _bootUtcSec = DateTime(F(__DATE__), F(__TIME__)).unixtime();
    _bootMs = millis();
    Serial.printf("[UTC] Fallback: bootUTC=%u\n", _bootUtcSec);
  }

  // ── Storage: LittleFS only (SD ใช้ไม่ได้ — SPI peripheral conflict กับ LoRa) ──
  if (!LittleFS.begin(true)) {
    Serial.println("[LFS]  Mount FAILED (formatted on first boot)");
  } else {
    Serial.println("[LFS]  Mounted OK");
    LittleFS.mkdir("/log");
    LittleFS.mkdir("/offline");
  }

  // Offline queue — ใช้ LittleFS เป็น backend (≈55 ชม. ที่ interval 60s)
  offlineInit(false);

  // ── LoRa init + load paired node registry ─────────────────────────────
  loraInit();
  loraRegistryLoad();

  // Network
  loadNetworkConfig(netConfig);
  uint8_t mode = 0;
  {
    // อ่าน mode จาก MCP23017 #3 (ถ้ามี)
    Wire.beginTransmission(MCP23_3_ADDR);
    if (Wire.endTransmission() == 0) {
      mcp_write(MCP23_3_ADDR, MCP23_IODIRA, 0x7F);
      mcp_write(MCP23_3_ADDR, MCP23_IODIRB, 0xFE);
      mcp_write(MCP23_3_ADDR, MCP23_GPPUA,  0x7F);
      mcp_write(MCP23_3_ADDR, MCP23_GPPUB,  0xFE);
      mode = (mcp_read(MCP23_3_ADDR, 0x12) >> 4) & 0x07;
    }
  }
  Serial.printf("[SYS]  Mode = %u (%s)\n", mode, getModeName(mode));

  if (modeHasLAN(mode))  initLAN();
  if (modeHasWiFi(mode)) initWifi();

  // MQTT setup
  const char* tbServer = netConfig.tb_server[0]
    ? netConfig.tb_server : "thingsboard.weaverbase.com";
  mqttSetup(tbServer);
  Serial.printf("[MQTT] Server: %s\n", tbServer);

  setupLogServer();

  pixels.setPixelColor(0, pixels.Color(0, 0, 255)); pixels.show(); // blue = ready
  Serial.println("\n[SYS]  Ready — loop starting");
  Serial.printf("[SYS]  Read every %ds | Send every %ds\n",
                READ_INTERVAL_MS/1000, SEND_INTERVAL_MS/1000);
  Serial.println("=========================================\n");
  Serial.flush();
}

// ════════════════════════════════════════════════════════════════════════
// Loop
// ════════════════════════════════════════════════════════════════════════
void loop() {
  unsigned long now = millis();

  // Handle captive portal / LAN web config
  handlePortal();
  handleLANServer();
  logServer.handleClient();   // LittleFS log viewer @ port 8080

  // Buttons (status / pairing / portal) + LoRa rx + pairing/polling
  buttonsLoop();
  loraGatewayLoop();

  // ── เช็คเน็ต + MQTT ทุก 10 วิ ─────────────────────────────────────────
  static unsigned long lastNet = 0;
  if (now - lastNet >= NET_CHECK_MS) {
    lastNet = now;
    checkNet();
    Serial.printf("[NET]  %s | MQTT: %s\n",
                  internet_connected ? "Online" : "Offline",
                  mqttIsConnected()  ? "Connected" : "Disconnected");
    Serial.flush();
  }

  // ── STEP 2: อ่าน sensor ทุก 5 วิ ──────────────────────────────────────
  static unsigned long lastRead = 0;
  if (now - lastRead >= READ_INTERVAL_MS) {
    lastRead = now;
    step2_read();
  }

  // ── STEP 3 + 4: ส่ง ThingsBoard ทุก 60 วิ ─────────────────────────────
  // step3 = sensor data → v1/gateway/telemetry (per parameter device)
  // step4 = port status + DHT20 + Board_Temp → v1/devices/me/telemetry
  // ทั้ง 2 ใช้ ts ร่วมกัน 1 ค่า (UTC) → ThingsBoard แปลงเป็นเวลาท้องถิ่นเอง
  static unsigned long lastSend = 0;
  if (now - lastSend >= SEND_INTERVAL_MS) {
    lastSend = now;
    uint64_t ts_ms = utcNowMs();   // millis()-based UTC — no RTC glitch
    step3_send(ts_ms);
    step4_send_device_telemetry(ts_ms);
  }

  delay(10);
}
