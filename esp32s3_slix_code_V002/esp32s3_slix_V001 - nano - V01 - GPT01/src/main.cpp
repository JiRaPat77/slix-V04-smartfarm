// ════════════════════════════════════════════════════════════════════════
// ESP32-S3 Nano LoRa Sensor Node — V01
//
// PCB identical to gateway. Roles:
//   • Read up to 12 RS485 sensors (configured in nano_config.h PORT_CONFIG[])
//   • If unpaired → broadcast HELLO every 5s until gateway sends ASSIGN
//   • If paired   → push packed DATA every NANO_SEND_INTERVAL_MS
//
// Timestamp: Bangkok (UTC+7) from DS3231 RTC, included in every DATA packet.
// Node ID: auto-assigned by gateway (stored in NVS, survives reboot).
// Factory reset: hold GPIO 45 for 10s → wipe NVS + restart (re-pair needed).
// ════════════════════════════════════════════════════════════════════════
#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <RTClib.h>
#include <Adafruit_NeoPixel.h>
#include <esp_task_wdt.h>
#include "modbus_rtu.h"
#include "sensor_types.h"
#include "lora_protocol.h"
#include "nano_config.h"
#include "nano_sensor.h"     // must come before nano_sender (defines NanoLastReading)
#include "nano_pairing.h"
#include "nano_sender.h"

// RTC — Bangkok ts is built from millis-based UTC + offset in nano_sensor.h
RTC_DS3231 rtc_nano;
uint32_t _nanoBootUtcSec = 0;   // UTC seconds at boot (from RTC)
uint32_t _nanoBootMs     = 0;   // millis() at boot reference

Adafruit_NeoPixel pixels(1, NANO_NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);

// ── DS3231 register helpers (for relay control via status bit 0) ────────
#define DS3231_ADDR  0x68
#define REG_CTRL     0x0E
#define REG_STATUS   0x0F

static void writeReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(DS3231_ADDR);
  Wire.write(reg); Wire.write(val);
  Wire.endTransmission();
}
static uint8_t readReg(uint8_t reg) {
  Wire.beginTransmission(DS3231_ADDR); Wire.write(reg); Wire.endTransmission();
  Wire.requestFrom((int)DS3231_ADDR, 1);
  return Wire.read();
}

// ── 12h ON / 5s OFF Power Cycle ─────────────────────────────────────────
const unsigned long ON_SECONDS  = 12UL * 3600UL;  // 12 ชม. เปิดไฟเลี้ยง sensor
const unsigned long OFF_SECONDS = 5;               // 5 วิ. ปิดไฟเลี้ยง sensor

// ── LED helpers ───────────────────────────────────────────────────────────
static void _ledIdle() {
  if (nanoIsPaired()) pixels.setPixelColor(0, pixels.Color(0, 80, 0));   // green = paired
  else                pixels.setPixelColor(0, pixels.Color(120, 80, 0)); // yellow = unpaired
  pixels.show();
}
static void _ledFlash(uint8_t r, uint8_t g, uint8_t b, int times) {
  for (int i = 0; i < times; i++) {
    pixels.setPixelColor(0, pixels.Color(r, g, b)); pixels.show(); delay(120);
    pixels.setPixelColor(0, 0); pixels.show(); delay(120);
  }
}

// ── Button (GPIO 45 = factory reset / 10s hold) ───────────────────────────
#define FACTORY_HOLD_MS  10000UL
static bool      _btnHeld  = false;
static uint32_t  _btnStart = 0;

static void _checkButton() {
  uint32_t now = millis();
  int b = digitalRead(NANO_BTN_RESET_PIN);
  if (b == LOW && !_btnHeld)  { _btnHeld = true;  _btnStart = now; }
  if (b == HIGH && _btnHeld)  {
    _btnHeld = false;
    if (now - _btnStart >= FACTORY_HOLD_MS) {
      _ledFlash(255, 0, 0, 5);
      nanoFactoryReset();
    }
  }
  if (b == LOW && _btnHeld && (now - _btnStart) >= FACTORY_HOLD_MS) {
    pixels.setPixelColor(0, ((now / 100) % 2) ? pixels.Color(255, 0, 0) : 0);
    pixels.show();
  }
}

// ── LoRa RX dispatch ─────────────────────────────────────────────────────
static void _loraRxPoll() {
  static char rxbuf[256];
  size_t n = loraReceiveMessage(rxbuf, sizeof(rxbuf), nullptr, nullptr);
  if (n == 0) return;
  char* cmd = nullptr, *payload = nullptr;
  if (!loraParseMessage(rxbuf, &cmd, &payload)) return;
  if      (strcmp(cmd, "ASSIGN")   == 0) nanoHandleAssign(payload);
  else if (strcmp(cmd, "REQUEST")  == 0) nanoHandleRequest(payload);
  else if (strcmp(cmd, "ACK_DATA") == 0) nanoHandleAckData(payload);
}

// ════════════════════════════════════════════════════════════════════════
// 12h ON / 5s OFF Power Cycle
// ════════════════════════════════════════════════════════════════════════
void restart_loop() {
  Serial.println("[SYS]  Entering 12h ON / 5s OFF power cycle");
  Serial.printf("[CFG]  ON=%luh  OFF=%lus\n", ON_SECONDS / 3600, OFF_SECONDS);
  Serial.flush();

  unsigned long cycleCount = 0;

  while (true) {
    cycleCount++;
    DateTime now = rtc_nano.now();

    Serial.println("========================================");
    Serial.printf("[SYS]  Cycle #%lu started\n", cycleCount);
    Serial.printf("[RTC]  %02d:%02d:%02d\n", now.hour(), now.minute(), now.second());
    Serial.printf("[CFG]  ON=%luh  OFF=%lus\n", ON_SECONDS / 3600, OFF_SECONDS);
    Serial.println("========================================");
    Serial.flush();

    // ── RELAY ON: เปิดไฟเลี้ยง sensor ──────────────────────────────────
    writeReg(REG_STATUS, readReg(REG_STATUS) | 0x01);
    pixels.setPixelColor(0, pixels.Color(0, 255, 0));
    pixels.show();

    Serial.printf("[RELAY] ON — counting %luh\n", ON_SECONDS / 3600);
    Serial.flush();

    // เปิดไฟ sensor ports ทุก port ที่กำหนด
    for (int i = 0; i < PORT_CONFIG_COUNT; i++) {
      _sensorPower(PORT_CONFIG[i].port, true);
    }

    unsigned long last_sec_tick = millis();
    for (unsigned long remaining = ON_SECONDS; remaining > 0; ) {
      unsigned long now_ms = millis();

      // Button + LoRa RX
      _checkButton();
      _loraRxPoll();

      // Sensor read + send (if paired)
      if (nanoIsPaired()) {
        nanoSensorReadLoop();
        nanoSendLoop();
        nanoSenderWatchdog();
      } else {
        nanoPairingLoop();
      }

      // ── 1-second tick: countdown + watchdog ────────────────────────────
      if (now_ms - last_sec_tick >= 1000) {
        last_sec_tick = now_ms;
        remaining--;
        esp_task_wdt_reset();

        if (remaining % 60 == 0 || remaining <= 10) {
          DateTime t = rtc_nano.now();
          unsigned long h = remaining / 3600;
          unsigned long m = (remaining % 3600) / 60;
          unsigned long s = remaining % 60;
          Serial.printf("[ON]  %02d:%02d:%02d | Relay=ON | %luh %lum %lus left\n",
                        t.hour(), t.minute(), t.second(), h, m, s);
          Serial.flush();
        }
      }

      // LED idle (not during button hold)
      static uint32_t lastLed = 0;
      if (now_ms - lastLed > 1000 && !_btnHeld) { lastLed = now_ms; _ledIdle(); }

      delay(1);
    }

    // ── RELAY OFF: ปิดไฟเลี้ยง sensor ──────────────────────────────────
    writeReg(REG_STATUS, readReg(REG_STATUS) & ~0x01);
    pixels.setPixelColor(0, pixels.Color(255, 0, 0));
    pixels.show();

    // ปิดไฟ sensor ports ทุก port
    for (int i = 0; i < PORT_CONFIG_COUNT; i++) {
      _sensorPower(PORT_CONFIG[i].port, false);
    }

    Serial.printf("[RELAY] OFF — waiting %lus\n", OFF_SECONDS);
    Serial.flush();

    for (unsigned long i = 0; i < OFF_SECONDS; i++) {
      delay(1000);
      esp_task_wdt_reset();
    }
  }
}

// ── Setup ─────────────────────────────────────────────────────────────────
void setup() {
  pixels.begin(); pixels.setBrightness(40);
  pixels.setPixelColor(0, pixels.Color(255, 165, 0)); pixels.show();  // orange = booting

  Serial.begin(115200);
  delay(2000);
  Serial.println("\n=========================================");
  Serial.println("[NANO] ESP32-S3 LoRa Sensor Node — V01");
  Serial.printf( "[NANO] %d sensor port(s) configured\n", PORT_CONFIG_COUNT);
  Serial.println("=========================================\n");
  Serial.flush();

  pinMode(NANO_BTN_RESET_PIN, INPUT_PULLUP);

  // I2C (DS3231 RTC + MCP23017)
  Wire.begin(NANO_I2C_SDA, NANO_I2C_SCL);
  Wire.setTimeOut(200);

  // RTC — read multiple times and take median to filter DS3231 I2C glitch
  // (on ESP32-S3, rtc.now().unixtime() can alternate between correct and wildly wrong)
  if (rtc_nano.begin(&Wire)) {
    if (rtc_nano.lostPower()) rtc_nano.adjust(DateTime(F(__DATE__), F(__TIME__)));
    // Read 7 samples, take median — robust against I2C glitch outliers
    const int N = 7;
    uint32_t samples[N];
    for (int i = 0; i < N; i++) {
      samples[i] = rtc_nano.now().unixtime();
      delay(20);
    }
    // Insertion sort
    for (int i = 1; i < N; i++) {
      uint32_t key = samples[i];
      int j = i - 1;
      while (j >= 0 && samples[j] > key) { samples[j+1] = samples[j]; j--; }
      samples[j+1] = key;
    }
    _nanoBootUtcSec = samples[N/2];   // median
    _nanoBootMs = millis();
    Serial.printf("[RTC]  bootUTC=%u (median of %d samples)\n", _nanoBootUtcSec, N);
    for (int i = 0; i < N; i++) Serial.printf("  sample[%d]=%u\n", i, samples[i]);
  } else {
    Serial.println("[RTC]  DS3231 not found!");
    // Fallback: use compile time as rough estimate
    _nanoBootUtcSec = DateTime(F(__DATE__), F(__TIME__)).unixtime();
    _nanoBootMs = millis();
    Serial.printf("[RTC]  Fallback: bootUTC=%u\n", _nanoBootUtcSec);
  }

  // RS485
  modbusInit();
  Serial.println("[SYS]  RS485 OK (GPIO43/44)");

  // MCP23017 + sensor port power
  nanoSensorInit();

  // LoRa
  if (!loraInit()) {
    Serial.println("[SYS]  LoRa FAILED — radio not responding");
    _ledFlash(255, 0, 0, 3);
  }

  // Pairing: load stored node ID or enter unpaired mode
  nanoPairingInit();

  // Print configured sensors
  for (int i = 0; i < PORT_CONFIG_COUNT; i++) {
    Serial.printf("[CFG]  Port%d → %s addr=%d instance=%s\n",
                  PORT_CONFIG[i].port,
                  SENSOR_TYPES[PORT_CONFIG[i].type].type_name,
                  PORT_CONFIG[i].address,
                  PORT_CONFIG[i].instance);
  }
  // Watchdog (30s) — ต้อง reset ใน restart_loop
  esp_task_wdt_init(30, true);
  esp_task_wdt_add(NULL);
  Serial.println("[WDT]  Watchdog enabled (30s)");
  Serial.flush();

  Serial.printf("[SYS]  Read every %ds | Send every %ds\n",
                NANO_READ_INTERVAL_MS / 1000, NANO_SEND_INTERVAL_MS / 1000);
  Serial.println("[SYS]  Entering 12h ON / 5s OFF power cycle\n");

  _ledIdle();

  // ── เข้าสู่โหมด 12h ON / 5s OFF (ไม่กลับมา loop) ──────────────────────
  restart_loop();
}

// ── Loop — ไม่ถูกเรียกเพราะ restart_loop() ทำงานใน while(true) ──────────
void loop() {
}
