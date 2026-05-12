// Plug & Play Deep Scan Engine
// Runs in a dedicated FreeRTOS task per port when hotplug detected.
// Sweeps all sensor types × addresses → identifies type & resolves collisions.
#pragma once
#include <Arduino.h>
#include "modbus_rtu.h"
#include "sensor_types.h"
#include "sensor_registry.h"

#define DEEP_SCAN_POWER_WAIT_MS 3000
#define DEEP_SCAN_PROBE_RETRIES 2

// Ports currently being scanned (bit mask, ports 1-16 → bits 0-15)
static volatile uint32_t _scanningPorts = 0;

// ── Global scan mutex — RS485 bus shared, only ONE port scanned at a time ──
// ป้องกัน sensor ที่ port หนึ่งตอบ query ของอีก port เพราะใช้ bus เดียวกัน
static SemaphoreHandle_t _scanBusMutex = nullptr;

inline void deepScanInit() {
  _scanBusMutex = xSemaphoreCreateMutex();
}

inline bool deepScanIsActive(uint8_t port) {
  if (port < 1 || port > 16) return false;
  return (_scanningPorts >> (port - 1)) & 1;
}

static void _scanSetActive(uint8_t port, bool active) {
  if (port < 1 || port > 16) return;
  uint32_t bit = 1UL << (port - 1);
  if (active) _scanningPorts |= bit;
  else        _scanningPorts &= ~bit;
}

// Forward declaration — implemented in main.cpp
extern void sensor_en_set(uint8_t num, bool on);

// Try to get a response from a sensor at (type, addr)
static bool _probe(SensorTypeID type, uint8_t addr) {
  const SensorTypeDef& t = SENSOR_TYPES[type];
  uint8_t raw[8];
  for (int i = 0; i < DEEP_SCAN_PROBE_RETRIES; i++) {
    if (modbusRead(addr, t.modbus_reg, t.reg_count, raw, t.baudrate) > 0)
      return true;
    delay(200);
  }
  return false;
}

// FreeRTOS task entry — one instance per port, deletes itself when done
struct _DeepScanArgs { uint8_t port; };

static void _deepScanTask(void* arg) {
  uint8_t port = ((const _DeepScanArgs*)arg)->port;
  delete (_DeepScanArgs*)arg;

  // รอรับ mutex — scan ทีละ port เท่านั้น (RS485 shared bus)
  if (_scanBusMutex && xSemaphoreTake(_scanBusMutex, pdMS_TO_TICKS(30000)) != pdTRUE) {
    Serial.printf("[SCAN] Port %d: timeout waiting for bus\n", port);
    _scanSetActive(port, false);
    vTaskDelete(nullptr);
    return;
  }

  Serial.printf("[SCAN] Port %d: starting deep scan\n", port);

  // ไฟทุก port เปิดอยู่แล้วตั้งแต่ boot (setup() เปิดทั้งหมด)
  // แต่ถ้า port นี้ถูกปิดไฟจาก OC event → เปิดคืนก่อน scan
  sensor_en_set(port, true);
  vTaskDelay(pdMS_TO_TICKS(DEEP_SCAN_POWER_WAIT_MS)); // รอ sensor stable

  SensorTypeID found_type = ST_UNKNOWN;
  uint8_t      found_addr = 0;

  // Sweep order: default address first (fastest), then full range
  for (int ti = 0; ti < ST_COUNT && found_type == ST_UNKNOWN; ti++) {
    SensorTypeID type = (SensorTypeID)ti;
    const SensorTypeDef& t = SENSOR_TYPES[type];

    // 1. Try factory default
    if (_probe(type, t.addr_default)) {
      found_type = type; found_addr = t.addr_default; break;
    }
    // 2. Sweep assigned range
    for (uint8_t a = t.addr_min; a <= t.addr_max; a++) {
      if (a == t.addr_default) continue;
      if (_probe(type, a)) { found_type = type; found_addr = a; break; }
    }
    // 3. Common factory reset addresses (50=0x32 default for rain/ultrasonic)
    static const uint8_t kFactory[] = {1, 2, 50, 11, 248};
    for (uint8_t fd : kFactory) {
      if (found_type != ST_UNKNOWN) break;
      if (_probe(type, fd)) { found_type = type; found_addr = fd; break; }
    }
  }

  if (found_type == ST_UNKNOWN) {
    Serial.printf("[SCAN] Port %d: no sensor found\n", port);
    sensor_en_set(port, false);
    _scanSetActive(port, false);
    if (_scanBusMutex) xSemaphoreGive(_scanBusMutex);
    vTaskDelete(nullptr);
    return;
  }

  Serial.printf("[SCAN] Port %d: found %s at addr %d\n",
                port, SENSOR_TYPES[found_type].type_name, found_addr);

  uint8_t final_addr = found_addr;
  char    instance[4];

  // Check if this was a previously seen sensor (restore instance number)
  ReservedEntry* reserved = registryFindReserved(found_addr, found_type);
  if (reserved) {
    strlcpy(instance, reserved->instance, sizeof(instance));
    Serial.printf("[SCAN] Port %d: restoring previous instance %s\n", port, instance);
  } else {
    // Check address collision with active local sensors
    if (registryAddressUsed(found_addr)) {
      uint8_t free_addr = registryFindFreeAddress(found_type);
      if (free_addr == 0) {
        Serial.printf("[SCAN] Port %d: address range full, skipping\n", port);
        sensor_en_set(port, false);
        _scanSetActive(port, false);
        if (_scanBusMutex) xSemaphoreGive(_scanBusMutex);
        vTaskDelete(nullptr);
        return;
      }
      Serial.printf("[SCAN] Port %d: collision, reassigning %d → %d\n",
                    port, found_addr, free_addr);
      if (!sensorChangeAddress(found_type, found_addr, free_addr)) {
        Serial.printf("[SCAN] Port %d: address change failed\n", port);
        sensor_en_set(port, false);
        _scanSetActive(port, false);
        if (_scanBusMutex) xSemaphoreGive(_scanBusMutex);
        vTaskDelete(nullptr);
        return;
      }
      final_addr = free_addr;
    }
    // Assign new instance number
    int cnt = registryCountType(found_type);
    snprintf(instance, sizeof(instance), "%02d", cnt + 1);
  }

  registryAdd(port, found_type, final_addr, instance);
  Serial.printf("[SCAN] Port %d: registered %s-%s addr=%d\n",
                port, SENSOR_TYPES[found_type].type_name, instance, final_addr);

  _scanSetActive(port, false);
  if (_scanBusMutex) xSemaphoreGive(_scanBusMutex); // คืน bus ให้ port อื่น
  vTaskDelete(nullptr);
}

// Trigger deep scan for a port (spawns background task)
inline void deepScanPort(uint8_t port) {
  if (port < 1 || port > 16) return;
  if (deepScanIsActive(port)) return; // already scanning

  _scanSetActive(port, true);
  auto* args = new _DeepScanArgs{port};
  BaseType_t ok = xTaskCreatePinnedToCore(
    _deepScanTask, "DeepScan", 8192, args, 1, nullptr, 0);
  if (ok != pdPASS) {
    Serial.printf("[SCAN] Port %d: failed to create task\n", port);
    delete args;
    _scanSetActive(port, false);
  }
}
