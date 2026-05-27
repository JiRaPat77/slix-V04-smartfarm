// RS485 / Modbus RTU Engine
// GPIO43 = UART1 TX (→ RS485 DI)
// GPIO44 = UART1 RX (← RS485 RO)
// No DE pin — auto-direction hardware on PCB
#pragma once
#include <Arduino.h>

#define RS485_TX          43
#define RS485_RX          44
#define RS485_BAUD_DEFAULT 9600
#define RS485_TIMEOUT_MS   500
#define RS485_RETRIES      5
#define RS485_RETRY_DELAY  200

static SemaphoreHandle_t _rs485Mutex = nullptr;
static uint32_t _rs485CurrentBaud   = 0;

// ── CRC16 Modbus ─────────────────────────────────────────────────────────
static uint16_t _modbusCRC(const uint8_t* d, size_t n) {
  uint16_t c = 0xFFFF;
  while (n--) {
    c ^= *d++;
    for (int i = 0; i < 8; i++)
      c = (c & 1) ? (c >> 1) ^ 0xA001 : c >> 1;
  }
  return c;
}

// ── Init ─────────────────────────────────────────────────────────────────
inline void modbusInit() {
  Serial1.begin(RS485_BAUD_DEFAULT, SERIAL_8N1, RS485_RX, RS485_TX);
  _rs485CurrentBaud = RS485_BAUD_DEFAULT;
  _rs485Mutex = xSemaphoreCreateMutex();
  delay(50);
}

// ── Internal: switch baudrate (no lock) ──────────────────────────────────
static void _rs485SetBaud(uint32_t baud) {
  if (baud == _rs485CurrentBaud) return;
  Serial1.end();
  delay(10);
  Serial1.begin(baud, SERIAL_8N1, RS485_RX, RS485_TX);
  delay(50);
  _rs485CurrentBaud = baud;
}

// ── Internal: FC03 Read Holding Registers (no lock, no retry) ────────────
static int _modbusReadRaw(uint8_t addr, uint16_t reg, uint8_t count,
                           uint8_t* out, uint32_t baud) {
  _rs485SetBaud(baud);

  uint8_t req[8] = {
    addr, 0x03,
    (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF),
    0x00, count
  };
  uint16_t crc = _modbusCRC(req, 6);
  req[6] = crc & 0xFF;
  req[7] = crc >> 8;

  // Flush any RX garbage / echo
  while (Serial1.available()) Serial1.read();
  Serial1.write(req, 8);
  Serial1.flush();

  uint8_t expected = 3 + count * 2 + 2;
  uint8_t resp[64] = {};
  uint8_t got = 0;
  unsigned long t = millis();
  while (got < expected && (millis() - t) < RS485_TIMEOUT_MS) {
    if (Serial1.available()) resp[got++] = (uint8_t)Serial1.read();
  }
  if (got < expected) return -1;

  uint16_t rcrc = _modbusCRC(resp, got - 2);
  if (rcrc != ((uint16_t)resp[got-2] | ((uint16_t)resp[got-1] << 8))) return -1;
  if (resp[0] != addr || resp[1] != 0x03) return -1;

  memcpy(out, resp + 3, count * 2);
  return count * 2;
}

// ── Internal: FC06 Write Single Register (no lock) ───────────────────────
static bool _modbusWriteRaw(uint8_t addr, uint16_t reg, uint16_t val, uint32_t baud) {
  _rs485SetBaud(baud);

  uint8_t req[8] = {
    addr, 0x06,
    (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF),
    (uint8_t)(val >> 8), (uint8_t)(val & 0xFF)
  };
  uint16_t crc = _modbusCRC(req, 6);
  req[6] = crc & 0xFF;
  req[7] = crc >> 8;

  while (Serial1.available()) Serial1.read();
  Serial1.write(req, 8);
  Serial1.flush();

  uint8_t resp[8] = {};
  uint8_t got = 0;
  unsigned long t = millis();
  while (got < 8 && (millis() - t) < RS485_TIMEOUT_MS) {
    if (Serial1.available()) resp[got++] = (uint8_t)Serial1.read();
  }
  if (got < 8) return false;
  uint16_t rcrc = _modbusCRC(resp, 6);
  return rcrc == ((uint16_t)resp[6] | ((uint16_t)resp[7] << 8)) && resp[0] == addr;
}

// ── Public: Read registers (thread-safe, with retry) ─────────────────────
inline int modbusRead(uint8_t addr, uint16_t reg, uint8_t count,
                       uint8_t* out, uint32_t baud = RS485_BAUD_DEFAULT) {
  if (!_rs485Mutex) return -1;
  if (xSemaphoreTake(_rs485Mutex, pdMS_TO_TICKS(1500)) != pdTRUE) return -1;
  int result = -1;
  for (int i = 0; i < RS485_RETRIES; i++) {
    result = _modbusReadRaw(addr, reg, count, out, baud);
    if (result > 0) break;
    delay(RS485_RETRY_DELAY);
  }
  xSemaphoreGive(_rs485Mutex);
  return result;
}

// ── Public: Write single register FC06 (thread-safe, flexible) ───────────
inline bool modbusWriteReg(uint8_t addr, uint16_t reg, uint16_t value,
                             uint32_t baud = RS485_BAUD_DEFAULT) {
  if (!_rs485Mutex) return false;
  if (xSemaphoreTake(_rs485Mutex, pdMS_TO_TICKS(1500)) != pdTRUE) return false;
  bool ok = _modbusWriteRaw(addr, reg, value, baud);
  xSemaphoreGive(_rs485Mutex);
  return ok;
}

// ── Public: Change sensor address (thread-safe) ───────────────────────────
// reg = register that holds the address setting (differs per sensor type)
inline bool modbusChangeAddress(uint8_t oldAddr, uint8_t newAddr,
                                  uint16_t reg,
                                  uint32_t baud = RS485_BAUD_DEFAULT) {
  if (!_rs485Mutex) return false;
  if (xSemaphoreTake(_rs485Mutex, pdMS_TO_TICKS(1500)) != pdTRUE) return false;
  bool ok = _modbusWriteRaw(oldAddr, reg, newAddr, baud);
  xSemaphoreGive(_rs485Mutex);
  if (ok) delay(500);
  return ok;
}
