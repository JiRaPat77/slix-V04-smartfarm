// ESP32-S3 + DS3231M Timer + MCP3423 ADC
// Cycle: 12h ON -> 5s OFF -> loop
// HW: DS3231M(0x68) MCP3423(0x6E) DHT20(0x38) SCD40(0x62)
//     MCP23017#1(0x27) MCP23017#2(0x20) MCP23017#3(0x21)

#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_NeoPixel.h>
#include <RTClib.h>
#include <esp_task_wdt.h>
#include <LoRa.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <Ethernet.h>
#include <WebServer.h>
#include "eth_config.h"
#include "captive_portal.h"
#include "lan_webconfig.h"

// ── Pin definitions ──────────────────────────────────────────────────
#ifndef NEOPIXEL_PIN
  #define NEOPIXEL_PIN 21
#endif
#define NUM_PIXELS    1
#define I2C_SDA       42
#define I2C_SCL       41
#define INT_PIN       4
#define BTN_STATUS_PIN      45  // Short press=Status LED, 10s hold=Factory Reset
#define BTN_PAIRING_PIN     46  // 5s hold=LoRa Pairing, 10s hold=Captive Portal

// ── LoRa SX1278 Pin definitions ───────────────────────────────
#define LORA_RST      33
#define LORA_NSS      34
#define LORA_MOSI     35
#define LORA_SCK      36
#define LORA_MISO     37
#define LORA_DIO0     39  // DIO0 for receive interrupt

// ── Timing ───────────────────────────────────────────────────────────
const unsigned long ON_SECONDS  = 12UL * 3600UL;
const unsigned long OFF_SECONDS = 5;

// ── DS3231M Registers ────────────────────────────────────────────────
#define DS3231_I2C   0x68
#define REG_CTRL     0x0E
#define REG_STATUS   0x0F

// ── MCP3423 Config ───────────────────────────────────────────────────
#define MCP3423_ADDR  0x6E
#define CFG_CH1_18BIT  0x70
#define CFG_CH2_18BIT  0x74

// ── DHT20 Config ─────────────────────────────────────────────────────
#define DHT20_ADDR     0x38

// ── SCD40 CO2 Sensor ─────────────────────────────────────────────────
#define SCD40_ADDR     0x62

// ── MCP23017 Addresses ───────────────────────────────────────────────
#define MCP23_1_ADDR   0x27
#define MCP23_2_ADDR   0x20
#define MCP23_3_ADDR   0x21

// MCP23017 Registers
#define MCP23_IODIRA   0x00
#define MCP23_IODIRB   0x01
#define MCP23_GPPUA    0x0C
#define MCP23_GPPUB    0x0D
#define MCP23_GPIOA    0x12
#define MCP23_GPIOB    0x13
#define MCP23_OLATA    0x14
#define MCP23_OLATB    0x15

// ── Globals ──────────────────────────────────────────────────────────
Adafruit_NeoPixel pixels(NUM_PIXELS, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);
RTC_DS3231 rtc;

uint8_t i2cFound[16];
int i2cCount = 0;

float battery_mV = 0.0;
float solar_mV = 0.0;

bool sensor_check[12];
uint8_t mode_value = 0;

// ── LoRa Global Variables ────────────────────────────────────────────
SPIClass lora_spi(HSPI);
bool lora_enabled = false;
char lora_rx_buf[256];
uint8_t lora_rx_len = 0;

// Node ID assigned by gateway
char node_id[8] = "N0";
bool node_id_assigned = false;

// ── W5500 Ethernet Globals ────────────────────────────────────────────
SPIClass eth_spi(FSPI);
NetworkConfig netConfig;
bool ap_mode_active = false;
WebServer portalServer(80);
WebServer lanServer(80);
bool portal_active = false;
bool lan_server_active = false;
bool eth_link_up = false;

// Forward declarations
void flashLoRaLED(uint8_t r, uint8_t g, uint8_t b, unsigned long duration_ms);
void updateLoRaLED();
void lora_gateway_loop();
void lora_node_loop();
void sendLoraMessage(const char* cmd, const char* payload);
bool parseLoraMessage(char* msg, char* cmd, char* payload);
void addNodeToList(const char* node_id, char* assigned_id);
void oc_check();
extern bool oc_value[16];

#define DISCOVERY_INTERVAL 5000
#define DISCOVERY_DURATION  60000
unsigned long discovery_start = 0;
bool discovery_active = false;

#define MAX_NODES 10
char node_ids[MAX_NODES][8];
int node_count = 0;

unsigned long last_request_time = 0;
#define REQUEST_INTERVAL 30000
int current_request_node = 0;

bool request_received = false;
bool ack_received = false;
unsigned long request_timeout = 0;
int retry_count = 0;
#define MAX_RETRIES 3

// ── LoRa LED Feedback ────────────────────────────────────────────────
int lora_led_state = 0;
unsigned long lora_led_flash_end = 0;
uint8_t lora_led_flash_r = 0;
uint8_t lora_led_flash_g = 0;
uint8_t lora_led_flash_b = 0;

char lora_last_rx_data[256];
int lora_last_rssi = 0;
int8_t lora_last_snr = 0;
unsigned long lora_last_rx_time = 0;
bool lora_new_data = false;

// ── Button & Pairing Mode Variables ────────────────────────────────────
#define PAIRING_HOLD_TIME   5000  // 5 seconds to enter/exit pairing mode
#define PORTAL_HOLD_TIME    10000 // 10 seconds to enter captive portal / factory reset
#define PAIRING_TIMEOUT     180000 // 3 minutes timeout for pairing

bool pairing_mode = false;
unsigned long pairing_start_time = 0;
unsigned long btn45_press_start = 0;
unsigned long btn46_press_start = 0;
bool btn45_pressed = false;
bool btn46_pressed = false;

// ── Status LED System Variables ─────────────────────────────────────────
#define STATUS_LED_PIN    15  // sensor_en15 on MCP23017_3
#define STATUS_LED_ON     HIGH
#define STATUS_LED_OFF    LOW

// Wifi/LAN status
bool wifi_connected = false;
bool lan_connected = false;
bool internet_connected = false;
bool internet_connecting = false;

// ══════════════════════════════════════════════════════════════════════
uint8_t readReg(uint8_t reg) {
  Wire.beginTransmission(DS3231_I2C);
  Wire.write(reg);
  Wire.endTransmission();
  Wire.requestFrom((int)DS3231_I2C, (int)1);
  return Wire.read();
}

void writeReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(DS3231_I2C);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

// ══════════════════════════════════════════════════════════════════════
void i2cBusRecovery() {
  pinMode(I2C_SCL, OUTPUT);
  pinMode(I2C_SDA, INPUT_PULLUP);
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

// ══════════════════════════════════════════════════════════════════════
void checkI2C() {
  Serial.println("[I2C]  Scanning...");
  i2cCount = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    uint8_t err = Wire.endTransmission(100);
    if (err == 0) {
      Serial.printf("[I2C]  [0x%02X]", addr);
      if (addr == 0x68) Serial.print("  DS3231M (RTC)");
      else if (addr == 0x6E) Serial.print("  MCP3423 (ADC)");
      else if (addr == 0x38) Serial.print("  DHT20 (Temp/Hum)");
      else if (addr == 0x27) Serial.print("  MCP23017 #1");
      else if (addr == 0x20) Serial.print("  MCP23017 #2");
      else if (addr == 0x21) Serial.print("  MCP23017 #3");
      Serial.println();
      if (i2cCount < 16) i2cFound[i2cCount++] = addr;
    }
  }
  Serial.printf("[I2C]  Found %d device(s)\n", i2cCount);
  Serial.flush();
}

String i2cAddrString() {
  String s = "I2C:";
  for (int i = 0; i < i2cCount; i++) {
    char buf[8];
    snprintf(buf, sizeof(buf), "[0x%02X]", i2cFound[i]);
    s += buf;
  }
  return s;
}

// ══════════════════════════════════════════════════════════════════════
// I2C Device Verification Functions
// ══════════════════════════════════════════════════════════════════════

struct I2CDevice {
  const char* name;
  uint8_t address;
  bool required;
};

const I2CDevice expected_i2c_devices[] = {
  {"DS3231M", 0x68, true},
  {"MCP3423", 0x6E, true},
  {"DHT20", 0x38, true},
  {"SCD40", 0x62, false},
  {"MCP23017#1", 0x27, true},
  {"MCP23017#2", 0x20, true},
  {"MCP23017#3", 0x21, true}
};

const int EXPECTED_I2C_DEVICES = sizeof(expected_i2c_devices) / sizeof(I2CDevice);

bool verifyI2CDevices() {
  Serial.println("[I2C]  Verifying I2C devices...");
  Serial.flush();
  
  bool all_verified = true;
  int found_count = 0;
  int missing_required = 0;
  
  for (int i = 0; i < EXPECTED_I2C_DEVICES; i++) {
    bool found = false;
    for (int j = 0; j < i2cCount; j++) {
      if (i2cFound[j] == expected_i2c_devices[i].address) {
        found = true;
        found_count++;
        Serial.printf("[I2C]  ✓ %s [0x%02X] - OK\n", 
                     expected_i2c_devices[i].name, 
                     expected_i2c_devices[i].address);
        break;
      }
    }
    
    if (!found) {
      if (expected_i2c_devices[i].required) {
        Serial.printf("[I2C]  ✗ %s [0x%02X] - MISSING (REQUIRED)\n", 
                     expected_i2c_devices[i].name, 
                     expected_i2c_devices[i].address);
        missing_required++;
        all_verified = false;
      } else {
        Serial.printf("[I2C]  - %s [0x%02X] - not found (optional)\n", 
                     expected_i2c_devices[i].name, 
                     expected_i2c_devices[i].address);
      }
    }
  }
  
  Serial.printf("[I2C]  Verification: %d/%d devices found\n", found_count, EXPECTED_I2C_DEVICES);
  
  if (missing_required > 0) {
    Serial.printf("[I2C]  WARNING: %d required device(s) missing!\n", missing_required);
    Serial.printf("[I2C]  System will continue but may have limited functionality.\n");
    // Flash red LED for 3 seconds to indicate missing devices
    for (int i = 0; i < 6; i++) {
      pixels.setPixelColor(0, pixels.Color(255, 0, 0));
      pixels.show();
      delay(250);
      pixels.setPixelColor(0, pixels.Color(0, 0, 0));
      pixels.show();
      delay(250);
    }
  } else {
    Serial.println("[I2C]  All required devices verified - OK");
    // Flash green LED for 1 second to indicate success
    for (int i = 0; i < 2; i++) {
      pixels.setPixelColor(0, pixels.Color(0, 255, 0));
      pixels.show();
      delay(250);
      pixels.setPixelColor(0, pixels.Color(0, 0, 0));
      pixels.show();
      delay(250);
    }
  }
  Serial.flush();
  
  return (missing_required == 0);
}

String getI2CStatusJSON() {
  DynamicJsonDocument doc(512);
  
  doc["node_id"] = node_id;
  doc["type"] = "i2c_check";
  doc["timestamp"] = (unsigned long)(millis() / 1000);
  doc["total_devices"] = EXPECTED_I2C_DEVICES;
  doc["expected"] = EXPECTED_I2C_DEVICES;
  doc["found"] = i2cCount;
  doc["status"] = (i2cCount == EXPECTED_I2C_DEVICES) ? "OK" : "WARNING";
  
  JsonArray devices = doc.createNestedArray("devices");
  
  for (int i = 0; i < EXPECTED_I2C_DEVICES; i++) {
    JsonObject device = devices.createNestedObject();
    device["name"] = expected_i2c_devices[i].name;
    char addr_str[8];
    snprintf(addr_str, sizeof(addr_str), "0x%02X", expected_i2c_devices[i].address);
    device["address"] = addr_str;
    
    bool found = false;
    for (int j = 0; j < i2cCount; j++) {
      if (i2cFound[j] == expected_i2c_devices[i].address) {
        found = true;
        break;
      }
    }
    device["status"] = found ? "OK" : "MISSING";
    device["required"] = expected_i2c_devices[i].required;
  }
  
  String jsonStr;
  serializeJson(doc, jsonStr);
  return jsonStr;
}

void sendI2CStatusJSON() {
  if (!lora_enabled || !node_id_assigned) {
    Serial.println("[LORA] Cannot send I2C status - LoRa not ready or node ID not assigned");
    return;
  }
  
  String jsonStr = getI2CStatusJSON();
  Serial.printf("[LORA] Sending I2C status: %s\n", jsonStr.c_str());
  sendLoraMessage("DATA", jsonStr.c_str());
}

// ══════════════════════════════════════════════════════════════════════
// JSON Data Sending Functions for Node Mode
// ══════════════════════════════════════════════════════════════════════

void sendNodeDataJSON(const char* dataType, const char* dataValue) {
  if (!lora_enabled || !node_id_assigned) {
    Serial.println("[LORA] Cannot send data - LoRa not ready or node ID not assigned");
    return;
  }
  
  DynamicJsonDocument doc(256);
  doc["node_id"] = node_id;
  doc["type"] = "data";
  doc["timestamp"] = (unsigned long)(millis() / 1000);
  doc["data_type"] = dataType;
  doc["value"] = dataValue;
  
  String jsonStr;
  serializeJson(doc, jsonStr);
  
  Serial.printf("[LORA] Sending node data: %s\n", jsonStr.c_str());
  sendLoraMessage("DATA", jsonStr.c_str());
}

void sendSensorJSON(float temp, float hum, float batt) {
  if (!lora_enabled || !node_id_assigned) {
    Serial.println("[LORA] Cannot send sensor data - LoRa not ready or node ID not assigned");
    return;
  }
  
  DynamicJsonDocument doc(384);
  doc["node_id"] = node_id;
  doc["type"] = "sensor";
  doc["timestamp"] = (unsigned long)(millis() / 1000);
  
  JsonObject sensors = doc.createNestedObject("sensors");
  sensors["temp"] = temp;
  sensors["humidity"] = hum;
  sensors["battery"] = batt;
  
  String jsonStr;
  serializeJson(doc, jsonStr);
  
  Serial.printf("[LORA] Sending sensor data: %s\n", jsonStr.c_str());
  sendLoraMessage("DATA", jsonStr.c_str());
}

void sendAlertJSON(const char* alertType, const char* message) {
  if (!lora_enabled || !node_id_assigned) {
    Serial.println("[LORA] Cannot send alert - LoRa not ready or node ID not assigned");
    return;
  }
  
  DynamicJsonDocument doc(256);
  doc["node_id"] = node_id;
  doc["type"] = "alert";
  doc["timestamp"] = (unsigned long)(millis() / 1000);
  doc["alert_type"] = alertType;
  doc["message"] = message;
  doc["priority"] = "high";
  
  String jsonStr;
  serializeJson(doc, jsonStr);
  
  Serial.printf("[LORA] Sending alert: %s\n", jsonStr.c_str());
  sendLoraMessage("DATA", jsonStr.c_str());
  flashLoRaLED(255, 0, 0, 500); // Flash red for alerts
}

void sendStatusJSON(const char* status, int activeSensors, float battery) {
  if (!lora_enabled || !node_id_assigned) {
    Serial.println("[LORA] Cannot send status - LoRa not ready or node ID not assigned");
    return;
  }
  
  DynamicJsonDocument doc(384);
  doc["node_id"] = node_id;
  doc["type"] = "status";
  doc["timestamp"] = (unsigned long)(millis() / 1000);
  doc["status"] = status;
  doc["active_sensors"] = activeSensors;
  doc["battery"] = battery;
  doc["uptime"] = (unsigned long)(millis() / 1000);
  
  String jsonStr;
  serializeJson(doc, jsonStr);
  
  Serial.printf("[LORA] Sending status: %s\n", jsonStr.c_str());
  sendLoraMessage("DATA", jsonStr.c_str());
}

void sendCustomJSON(const char* jsonPayload) {
  if (!lora_enabled || !node_id_assigned) {
    Serial.println("[LORA] Cannot send custom JSON - LoRa not ready or node ID not assigned");
    return;
  }
  
  Serial.printf("[LORA] Sending custom JSON: %s\n", jsonPayload);
  sendLoraMessage("DATA", jsonPayload);
}

// Helper function to read temperature and humidity from DHT20
void getDHT20Readings(float* temp, float* hum) {
  Wire.beginTransmission(DHT20_ADDR);
  Wire.write(0xAC);
  Wire.write(0x33);
  Wire.write(0x00);
  if (Wire.endTransmission() != 0) {
    *temp = NAN;
    *hum = NAN;
    return;
  }
  delay(80);
  Wire.requestFrom((int)DHT20_ADDR, (int)7);
  if (Wire.available() < 7) {
    *temp = NAN;
    *hum = NAN;
    return;
  }
  uint8_t d[7];
  for (int i = 0; i < 7; i++) d[i] = Wire.read();
  if (d[0] & 0x80) {
    *temp = NAN;
    *hum = NAN;
    return;
  }
  uint32_t rawHum = ((uint32_t)d[1] << 12) | ((uint32_t)d[2] << 4) | (d[3] >> 4);
  *hum = (float)rawHum / 1048576.0 * 100.0;
  uint32_t rawTemp = ((uint32_t)(d[3] & 0x0F) << 16) | ((uint32_t)d[4] << 8) | d[5];
  *temp = (float)rawTemp / 1048576.0 * 200.0 - 50.0;
}

// ══════════════════════════════════════════════════════════════════════
static void mcp3423WriteConfig(uint8_t config) {
  Wire.beginTransmission(MCP3423_ADDR);
  Wire.write(config);
  Wire.endTransmission();
}

static float mcp3423ReadChannel(uint8_t channel) {
  uint8_t config = (channel == 1) ? CFG_CH1_18BIT : CFG_CH2_18BIT;
  mcp3423WriteConfig(config);
  delay(300);
  Wire.requestFrom((int)MCP3423_ADDR, (int)4);
  if (Wire.available() < 4) return NAN;
  uint8_t d[4];
  for (int i = 0; i < 4; i++) d[i] = Wire.read();
  int32_t raw = ((int32_t)d[0] << 16) | ((int32_t)d[1] << 8) | d[2];
  if (raw & 0x020000) raw |= 0xFFFC0000;
  if (d[3] & 0x80) return NAN;
  float value = (float)raw * 2.048 / 131072.0 * 1000.0;
  if (channel == 1) solar_mV = value;
  else if (channel == 2) battery_mV = value;
  return value;
}

void read_power() {
  mcp3423ReadChannel(1);
  mcp3423ReadChannel(2);
  if (isnan(solar_mV)) {
    Serial.printf("[PWR]  Solar:   --- not ready ---\n");
  } else {
    Serial.printf("[PWR]  Solar:   %+.2f mV  (%.4f V)\n", solar_mV, solar_mV / 1000.0);
  }
  if (isnan(battery_mV)) {
    Serial.printf("[PWR]  Battery: --- not ready ---\n");
  } else {
    Serial.printf("[PWR]  Battery: %+.2f mV  (%.4f V)\n", battery_mV, battery_mV / 1000.0);
  }
  Serial.flush();
}

// ══════════════════════════════════════════════════════════════════════
void onboard_temp_hum() {
  Wire.beginTransmission(DHT20_ADDR);
  Wire.write(0xAC);
  Wire.write(0x33);
  Wire.write(0x00);
  if (Wire.endTransmission() != 0) {
    Serial.printf("[DHT]  Error: cannot trigger measurement\n");
    return;
  }
  delay(80);
  Wire.requestFrom((int)DHT20_ADDR, (int)7);
  if (Wire.available() < 7) {
    Serial.printf("[DHT]  Error: not enough data\n");
    return;
  }
  uint8_t d[7];
  for (int i = 0; i < 7; i++) d[i] = Wire.read();
  if (d[0] & 0x80) {
    Serial.printf("[DHT]  Error: busy (status=0x%02X)\n", d[0]);
    return;
  }
  uint32_t rawHum = ((uint32_t)d[1] << 12) | ((uint32_t)d[2] << 4) | (d[3] >> 4);
  float humidity = (float)rawHum / 1048576.0 * 100.0;
  uint32_t rawTemp = ((uint32_t)(d[3] & 0x0F) << 16) | ((uint32_t)d[4] << 8) | d[5];
  float temperature = (float)rawTemp / 1048576.0 * 200.0 - 50.0;
  Serial.printf("[DHT]  Temp: %.1f C  Hum: %.1f%%\n", temperature, humidity);
  Serial.flush();
}

// ══════════════════════════════════════════════════════════════════════
static uint8_t mcp23Read(uint8_t addr, uint8_t reg) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.endTransmission();
  Wire.requestFrom((int)addr, (int)1);
  return Wire.read();
}

static void mcp23Write(uint8_t addr, uint8_t reg, uint8_t val) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

void mcp23017_1_init() {
  mcp23Write(MCP23_1_ADDR, MCP23_IODIRA, 0xF0);
  mcp23Write(MCP23_1_ADDR, MCP23_IODIRB, 0xF0);
  mcp23Write(MCP23_1_ADDR, MCP23_GPPUA, 0x00);
  mcp23Write(MCP23_1_ADDR, MCP23_GPPUB, 0x00);
  mcp23Write(MCP23_1_ADDR, MCP23_OLATA, 0x00);
  mcp23Write(MCP23_1_ADDR, MCP23_OLATB, 0x00);
  Serial.println("[MCP23] MCP23017 #1 (0x27) initialized");
  Serial.flush();
}

void mcp23017_2_init() {
  mcp23Write(MCP23_2_ADDR, MCP23_IODIRA, 0xF0);
  mcp23Write(MCP23_2_ADDR, MCP23_IODIRB, 0xF0);
  mcp23Write(MCP23_2_ADDR, MCP23_GPPUA, 0x00);
  mcp23Write(MCP23_2_ADDR, MCP23_GPPUB, 0x00);
  mcp23Write(MCP23_2_ADDR, MCP23_OLATA, 0x00);
  mcp23Write(MCP23_2_ADDR, MCP23_OLATB, 0x00);
  Serial.println("[MCP23] MCP23017 #2 (0x20) initialized");
  Serial.flush();
}

void mcp23017_3_init() {
  mcp23Write(MCP23_3_ADDR, MCP23_IODIRA, 0x7F);
  mcp23Write(MCP23_3_ADDR, MCP23_IODIRB, 0xFE);  // Bit 0 = output for status LED, rest = input
  mcp23Write(MCP23_3_ADDR, MCP23_GPPUA, 0x00);
  mcp23Write(MCP23_3_ADDR, MCP23_GPPUB, 0x00);
  mcp23Write(MCP23_3_ADDR, MCP23_OLATA, 0x00);
  mcp23Write(MCP23_3_ADDR, MCP23_OLATB, 0x00);  // Initialize status LED to OFF
  Serial.println("[MCP23] MCP23017 #3 (0x21) initialized");
  Serial.println("[MCP23] Status LED (PORTB bit 0) configured");
  Serial.flush();
}

void led_sys(bool on) {
  uint8_t olat = mcp23Read(MCP23_3_ADDR, MCP23_OLATA);
  if (on) mcp23Write(MCP23_3_ADDR, MCP23_OLATA, olat | 0x80);
  else    mcp23Write(MCP23_3_ADDR, MCP23_OLATA, olat & ~0x80);
}

void sensor_en_set(uint8_t num, bool on) {
  uint8_t addr, reg, bit;
  if (num >= 1 && num <= 4)       { addr = MCP23_1_ADDR; reg = MCP23_OLATB; bit = num - 1; }
  else if (num >= 5 && num <= 8)  { addr = MCP23_1_ADDR; reg = MCP23_OLATA; bit = num - 5; }
  else if (num >= 9 && num <= 12) { addr = MCP23_2_ADDR; reg = MCP23_OLATB; bit = num - 9; }
  else if (num >= 13 && num <= 16){ addr = MCP23_2_ADDR; reg = MCP23_OLATA; bit = num - 13; }
  else return;
  uint8_t olat = mcp23Read(addr, reg);
  if (on) mcp23Write(addr, reg, olat | (1 << bit));
  else    mcp23Write(addr, reg, olat & ~(1 << bit));
}

// ══════════════════════════════════════════════════════════════════════
// Status LED Control Functions (sensor_out15 on MCP23017_3)
// ══════════════════════════════════════════════════════════════════════

void setStatusLED(bool on) {
  // Control status LED on MCP23017_3 PORTB bit 0
  // This assumes sensor_out15 is connected to PORTB pin 0
  uint8_t olatb = mcp23Read(MCP23_3_ADDR, MCP23_OLATB);
  if (on) {
    mcp23Write(MCP23_3_ADDR, MCP23_OLATB, olatb | 0x01);
  } else {
    mcp23Write(MCP23_3_ADDR, MCP23_OLATB, olatb & ~0x01);
  }
}

void blinkStatusLED(int count, unsigned long on_ms, unsigned long off_ms) {
  for (int i = 0; i < count; i++) {
    setStatusLED(true);
    delay(on_ms);
    setStatusLED(false);
    if (i < count - 1) {  // Don't delay after last blink
      delay(off_ms);
    }
  }
}

// ══════════════════════════════════════════════════════════════════════
// Status Display Functions
// ══════════════════════════════════════════════════════════════════════

void showInternetStatus() {
  Serial.println("[STATUS] ===== Internet Status =====");
  if (modeHasWiFi(mode_value)) {
    if (wifi_connected) {
      Serial.printf("[STATUS] Internet: CONNECTED (WiFi) IP: %s\n", WiFi.localIP().toString().c_str());
      blinkStatusLED(1, 500, 500);
    } else if (internet_connecting) {
      Serial.println("[STATUS] Internet: CONNECTING (WiFi)");
      blinkStatusLED(1, 100, 100);
    } else {
      Serial.println("[STATUS] Internet: DISCONNECTED (WiFi)");
      blinkStatusLED(1, 500, 500);
    }
  } else if (modeHasLAN(mode_value)) {
    if (lan_connected) {
      Serial.printf("[STATUS] Internet: CONNECTED (LAN) IP: %s\n", Ethernet.localIP().toString().c_str());
      blinkStatusLED(1, 500, 500);
    } else if (internet_connecting) {
      Serial.println("[STATUS] Internet: CONNECTING (LAN)");
      blinkStatusLED(1, 100, 100);
    } else {
      Serial.println("[STATUS] Internet: DISCONNECTED (LAN)");
      blinkStatusLED(1, 500, 500);
    }
  } else {
    Serial.println("[STATUS] Internet: NOT AVAILABLE (this mode has no WiFi/LAN)");
    blinkStatusLED(1, 200, 200);
  }
  Serial.flush();
  delay(1000);
}

void showI2CStatus() {
  Serial.println("[STATUS] ===== I2C Status =====");
  Serial.printf("[STATUS] I2C devices found: %d/%d\n", i2cCount, EXPECTED_I2C_DEVICES);
  
  int required_devices = 6;  // 6 required devices (excluding SCD40)
  
  if (i2cCount >= EXPECTED_I2C_DEVICES) {
    Serial.println("[STATUS] I2C: ALL DEVICES OK (including optional)");
    blinkStatusLED(2, 500, 500);
  } else if (i2cCount >= required_devices) {
    Serial.println("[STATUS] I2C: OK (missing optional device)");
    blinkStatusLED(2, 500, 500);
  } else {
    Serial.printf("[STATUS] I2C: ERROR - Missing %d required devices\n", required_devices - i2cCount);
    blinkStatusLED(2, 500, 500);
  }
  
  Serial.flush();
  delay(1000);
}

void showLoRaStatus() {
  Serial.println("[STATUS] ===== LoRa Status =====");
  
  if (!lora_enabled) {
    Serial.println("[STATUS] LoRa: DISABLED");
    blinkStatusLED(3, 500, 500);
  } else if (pairing_mode) {
    Serial.println("[STATUS] LoRa: PAIRING MODE");
    blinkStatusLED(3, 500, 500);
  } else if (modeIsGateway(mode_value)) {
    // Gateway mode
    if (node_count > 0) {
      Serial.printf("[STATUS] LoRa Gateway: CONNECTED (%d nodes)\n", node_count);
      blinkStatusLED(3, 500, 500);
    } else {
      Serial.println("[STATUS] LoRa Gateway: WAITING FOR NODES");
      blinkStatusLED(3, 500, 500);
    }
  } else if (mode_value == MODE_LORA_NODE) {
    // Node mode
    if (node_id_assigned) {
      Serial.printf("[STATUS] LoRa Node: CONNECTED (ID: %s)\n", node_id);
      blinkStatusLED(3, 500, 500);
    } else {
      Serial.println("[STATUS] LoRa Node: NOT PAIRED");
      blinkStatusLED(3, 500, 500);
    }
  } else {
    Serial.println("[STATUS] LoRa: UNKNOWN MODE");
    blinkStatusLED(3, 500, 500);
  }
  
  Serial.flush();
  delay(1000);
}

void showOvercurrentStatus() {
  Serial.println("[STATUS] ===== Overcurrent Status =====");
  
  bool has_overcurrent = false;
  int oc_count = 0;
  
  for (int i = 0; i < 16; i++) {
    if (oc_value[i]) {
      has_overcurrent = true;
      oc_count++;
      Serial.printf("[STATUS] OC_%d detected\n", i + 1);
    }
  }
  
  if (has_overcurrent) {
    Serial.printf("[STATUS] OVERCURRENT DETECTED (%d channels)\n", oc_count);
    Serial.println("[STATUS] *** WARNING: CHECK SENSOR CONNECTIONS ***");
    blinkStatusLED(4, 500, 500);
  } else {
    Serial.println("[STATUS] Overcurrent: NONE");
    blinkStatusLED(4, 500, 500);
  }
  
  Serial.flush();
  delay(1000);
}

void showAllStatuses() {
  Serial.println("========================================");
  Serial.println("[STATUS] ===== SYSTEM STATUS CHECK =====");
  Serial.println("========================================");
  Serial.flush();
  
  // Update overcurrent values before checking
  oc_check();
  
  // 1. Internet Status (1 blink)
  showInternetStatus();
  
  // 2. I2C Status (2 blinks)
  showI2CStatus();
  
  // 3. LoRa Status (3 blinks)
  showLoRaStatus();
  
  // 4. Overcurrent Status (4 blinks)
  showOvercurrentStatus();
  
  Serial.println("========================================");
  Serial.println("[STATUS] ===== END OF STATUS CHECK =====");
  Serial.println("========================================");
  Serial.flush();
  
  // Ensure LED is OFF after display
  setStatusLED(false);
}

// ══════════════════════════════════════════════════════════════════════
// W5500 Ethernet (LAN) & WiFi Initialization
// ══════════════════════════════════════════════════════════════════════

void initLAN() {
  Serial.println("[LAN] Initializing W5500 via FSPI...");
  Serial.printf("[LAN] Pins: SCLK=%d MISO=%d MOSI=%d CS=%d INT=%d RST=%d\n",
                ETH_SCLK, ETH_MISO, ETH_MOSI, ETH_CS, ETH_INT, ETH_RST);
  Serial.flush();
  eth_spi.begin(ETH_SCLK, ETH_MISO, ETH_MOSI, ETH_CS);
  pinMode(ETH_RST, OUTPUT);
  digitalWrite(ETH_RST, LOW); delay(20);
  digitalWrite(ETH_RST, HIGH); delay(150);
  Ethernet.init(ETH_CS);
  byte mac[] = {0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED};
  if (netConfig.ip_mode == 1) {
    IPAddress ip, sn, gw, dns;
    ip.fromString(netConfig.static_ip); sn.fromString(netConfig.subnet);
    gw.fromString(netConfig.gateway); dns.fromString(netConfig.dns);
    Ethernet.begin(mac, ip, dns, gw, sn);
    Serial.printf("[LAN] Static IP: %s\n", netConfig.static_ip);
  } else {
    Serial.println("[LAN] Attempting DHCP...");
    if (Ethernet.begin(mac, 10000) == 0) {
      Serial.println("[LAN] DHCP FAILED");
      lan_connected = false; Serial.flush(); return;
    }
  }
  lan_connected = true; internet_connected = true;
  Serial.printf("[LAN] Connected! IP: %s\n", Ethernet.localIP().toString().c_str());
  Serial.flush();
  startLANWebServer();
}

void checkLANStatus() {
  switch (Ethernet.maintain()) {
    case 1: Serial.println("[LAN] DHCP renew failed"); lan_connected = false; break;
    case 2: Serial.printf("[LAN] DHCP renew OK, IP: %s\n", Ethernet.localIP().toString().c_str()); lan_connected = true; break;
    case 3: Serial.println("[LAN] DHCP rebind failed"); lan_connected = false; break;
    case 4: Serial.printf("[LAN] DHCP rebind OK, IP: %s\n", Ethernet.localIP().toString().c_str()); lan_connected = true; break;
  }
}

void initWifi() {
  Serial.println("[WIFI] Initializing WiFi..."); Serial.flush();
  if (netConfig.wifi_ssid[0] == '\0') {
    Serial.println("[WIFI] No SSID - starting captive portal");
    startCaptivePortal(); return;
  }
  WiFi.mode(WIFI_STA);
  WiFi.begin(netConfig.wifi_ssid, netConfig.wifi_pass);
  internet_connecting = true;
  Serial.printf("[WIFI] Connecting to '%s'...\n", netConfig.wifi_ssid); Serial.flush();
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) { delay(500); attempts++; }
  if (WiFi.status() == WL_CONNECTED) {
    wifi_connected = true; internet_connected = true; internet_connecting = false;
    Serial.printf("[WIFI] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    wifi_connected = false; internet_connected = false; internet_connecting = false;
    Serial.println("[WIFI] FAILED - starting captive portal"); startCaptivePortal();
  }
  Serial.flush();
}

void checkWifiStatus() {
  wifi_connected = (WiFi.status() == WL_CONNECTED);
  internet_connected = wifi_connected;
}

bool oc_value[16];

void oc_check() {
  uint8_t gpb1 = mcp23Read(MCP23_1_ADDR, MCP23_GPIOB);
  uint8_t gpa1 = mcp23Read(MCP23_1_ADDR, MCP23_GPIOA);
  uint8_t gpb2 = mcp23Read(MCP23_2_ADDR, MCP23_GPIOB);
  uint8_t gpa2 = mcp23Read(MCP23_2_ADDR, MCP23_GPIOA);
  for (int i = 0; i < 4; i++) {
    oc_value[i]      = (gpb1 >> (4 + i)) & 1;
    oc_value[4 + i]  = (gpa1 >> (4 + i)) & 1;
    oc_value[8 + i]  = (gpb2 >> (4 + i)) & 1;
    oc_value[12 + i] = (gpa2 >> (4 + i)) & 1;
  }
  Serial.printf("[OC]   #1: %d%d%d%d %d%d%d%d  |  #2: %d%d%d%d %d%d%d%d\n",
                oc_value[0], oc_value[1], oc_value[2], oc_value[3],
                oc_value[4], oc_value[5], oc_value[6], oc_value[7],
                oc_value[8], oc_value[9], oc_value[10], oc_value[11],
                oc_value[12], oc_value[13], oc_value[14], oc_value[15]);
  Serial.flush();
}

void mode_check() {
  uint8_t gpioa = mcp23Read(MCP23_3_ADDR, MCP23_GPIOA);
  mode_value = (gpioa >> 4) & 0x07;
  Serial.printf("[MODE] mode=%u (GPA4=%d GPA5=%d GPA6=%d)\n",
                mode_value,
                (gpioa >> 4) & 1,
                (gpioa >> 5) & 1,
                (gpioa >> 6) & 1);
  Serial.flush();
}

// ══════════════════════════════════════════════════════════════════════
// Pairing Mode Functions
// ══════════════════════════════════════════════════════════════════════

void checkButtons() {
  unsigned long now = millis();
  
  // ── GPIO 45: Short press=Status LED, 10s hold=Factory Reset ──
  int btn45 = digitalRead(BTN_STATUS_PIN);
  if (btn45 == LOW && !btn45_pressed) {
    btn45_pressed = true; btn45_press_start = now;
    Serial.println("[BTN45] Pressed"); Serial.flush();
  }
  if (btn45 == HIGH && btn45_pressed) {
    btn45_pressed = false;
    unsigned long dur = now - btn45_press_start;
    if (dur >= PORTAL_HOLD_TIME) {
      Serial.println("[BTN45] FACTORY RESET!"); Serial.flush();
      Preferences prefs;
      prefs.begin(NVS_NAMESPACE, false);
      prefs.clear();
      prefs.end();
      for (int i = 0; i < 5; i++) {
        pixels.setPixelColor(0, pixels.Color(255, 0, 0)); pixels.show(); delay(200);
        pixels.setPixelColor(0, pixels.Color(0, 0, 0));    pixels.show(); delay(200);
      }
      ESP.restart();
    } else {
      Serial.println("[BTN45] Short press - system status"); Serial.flush();
      showAllStatuses();
    }
  }
  if (btn45 == LOW && btn45_pressed) {
    unsigned long hold = now - btn45_press_start;
    if (hold >= PORTAL_HOLD_TIME) {
      pixels.setPixelColor(0, (now/100)%2 ? pixels.Color(255,0,0) : pixels.Color(0,0,0));
      pixels.show();
    }
  }
  
  // ── GPIO 46: 5s hold=LoRa Pairing, 10s hold=Captive Portal ──
  int btn46 = digitalRead(BTN_PAIRING_PIN);
  if (btn46 == LOW && !btn46_pressed) {
    btn46_pressed = true; btn46_press_start = now;
    Serial.println("[BTN46] Pressed"); Serial.flush();
  }
  if (btn46 == HIGH && btn46_pressed) {
    btn46_pressed = false;
    unsigned long dur = now - btn46_press_start;
    if (dur >= PORTAL_HOLD_TIME) {
      Serial.println("[BTN46] CAPTIVE PORTAL / LAN CONFIG"); Serial.flush();
      if (modeHasWiFi(mode_value)) startCaptivePortal();
      else if (modeHasLAN(mode_value)) startLANWebServer();
    } else if (dur >= PAIRING_HOLD_TIME) {
      if (modeHasLoRa(mode_value)) {
        pairing_mode = !pairing_mode;
        if (pairing_mode) {
          pairing_start_time = now;
          Serial.println("[PAIR] LoRa Pairing mode ENTERED"); Serial.flush();
          for (int i = 0; i < 3; i++) {
            pixels.setPixelColor(0, pixels.Color(255,0,255)); pixels.show(); delay(100);
            pixels.setPixelColor(0, pixels.Color(0,0,0));     pixels.show(); delay(100);
          }
          if (mode_value == MODE_LORA_NODE) { node_id_assigned = false; strcpy(node_id, "N0"); }
        } else {
          Serial.println("[PAIR] LoRa Pairing mode EXITED"); Serial.flush();
          for (int i = 0; i < 2; i++) {
            pixels.setPixelColor(0, pixels.Color(0,0,255)); pixels.show(); delay(100);
            pixels.setPixelColor(0, pixels.Color(0,0,0));    pixels.show(); delay(100);
          }
        }
      }
    }
  }
  if (btn46 == LOW && btn46_pressed) {
    unsigned long hold = now - btn46_press_start;
    if (hold >= PORTAL_HOLD_TIME) {
      pixels.setPixelColor(0, (now/100)%2 ? pixels.Color(255,165,0) : pixels.Color(0,0,0));
      pixels.show();
    } else if (hold >= PAIRING_HOLD_TIME) {
      pixels.setPixelColor(0, (now/100)%2 ? pixels.Color(255,0,255) : pixels.Color(0,0,0));
      pixels.show();
    }
  }
}

void lora_pairing_mode_node() {
  unsigned long now = millis();
  
  // Check timeout
  if (now - pairing_start_time >= PAIRING_TIMEOUT) {
    Serial.println("[PAIR] Timeout - exiting pairing mode");
    pairing_mode = false;
    Serial.flush();
    return;
  }
  
  int pktSize = LoRa.parsePacket();
  if (pktSize > 0) {
    String raw = "";
    lora_rx_len = 0;
    while (LoRa.available()) {
      char c = LoRa.read();
      if (lora_rx_len < sizeof(lora_rx_buf) - 1) {
        lora_rx_buf[lora_rx_len++] = c;
      }
      raw += c;
    }
    lora_rx_buf[lora_rx_len] = '\0';

    int rssi = LoRa.packetRssi();
    Serial.printf("[LORA] RX: %d bytes | RSSI: %d dBm | %s\n", lora_rx_len, rssi, raw.c_str());
    flashLoRaLED(0, 255, 255, 100);

    char cmd[16], payload[64];
    if (parseLoraMessage(lora_rx_buf, cmd, payload)) {
      if (strcmp(cmd, "DISCOVER") == 0) {
        Serial.println("[PAIR] Gateway DISCOVER received - sending HELLO");
        Serial.flush();
        sendLoraMessage("HELLO", "PAIRING_NODE");
        LoRa.receive();
        return;
      }

      if (strcmp(cmd, "ASSIGN") == 0) {
        // Receive and store assigned ID from gateway
        strncpy(node_id, payload, 7);
        node_id[7] = '\0';
        node_id_assigned = true;
        Serial.printf("[PAIR] Assigned ID received: %s\n", node_id);
        Serial.println("[PAIR] Pairing SUCCESS - exiting pairing mode");
        Serial.flush();
        pairing_mode = false;
        
        // Flash green LED to indicate success
        for (int i = 0; i < 3; i++) {
          pixels.setPixelColor(0, pixels.Color(0, 255, 0));
          pixels.show();
          delay(150);
          pixels.setPixelColor(0, pixels.Color(0, 0, 0));
          pixels.show();
          delay(150);
        }
        LoRa.receive();
        return;
      }
    }

    LoRa.receive();
  }
}

void lora_pairing_mode_gateway() {
  static unsigned long last_discover_send = 0;
  unsigned long now = millis();
  
  // Check timeout (optional - gateway can stay in pairing mode indefinitely)
  // if (now - pairing_start_time >= PAIRING_TIMEOUT) {
  //   Serial.println("[PAIR] Timeout - exiting pairing mode");
  //   pairing_mode = false;
  //   Serial.flush();
  //   return;
  // }
  
  // Send DISCOVER every 5 seconds
  if (now - last_discover_send >= DISCOVERY_INTERVAL) {
    last_discover_send = now;
    Serial.printf("[PAIR] Sending DISCOVER packet...\n");
    Serial.flush();
    sendLoraMessage("DISCOVER", "");
  }

  int pktSize = LoRa.parsePacket();
  if (pktSize > 0) {
    String raw = "";
    lora_rx_len = 0;
    while (LoRa.available()) {
      char c = LoRa.read();
      if (lora_rx_len < sizeof(lora_rx_buf) - 1) {
        lora_rx_buf[lora_rx_len++] = c;
      }
      raw += c;
    }
    lora_rx_buf[lora_rx_len] = '\0';

    int rssi = LoRa.packetRssi();
    Serial.printf("[LORA] RX: %d bytes | RSSI: %d dBm | %s\n", lora_rx_len, rssi, raw.c_str());
    flashLoRaLED(0, 255, 255, 100);

    char cmd[16], payload[64];
    if (parseLoraMessage(lora_rx_buf, cmd, payload)) {
      if (strcmp(cmd, "HELLO") == 0) {
        char assigned_id[8];
        addNodeToList(payload, assigned_id);
        Serial.printf("[PAIR] Node discovered: %s -> Assigned ID: %s\n", payload, assigned_id);
        Serial.flush();
        // Send assigned ID back to node
        sendLoraMessage("ASSIGN", assigned_id);
      }
    }

    LoRa.receive();
  }
}

void sensor_check_func() {
  uint8_t gpiob = mcp23Read(MCP23_3_ADDR, MCP23_GPIOB);
  uint8_t gpioa = mcp23Read(MCP23_3_ADDR, MCP23_GPIOA);
  for (int i = 0; i < 8; i++) sensor_check[i] = (gpiob >> i) & 1;
  for (int i = 0; i < 4; i++) sensor_check[8 + i] = (gpioa >> i) & 1;
  Serial.printf("[SEN]  S01-08: ");
  for (int i = 0; i < 8; i++) Serial.printf("%d", sensor_check[i]);
  Serial.printf("  |  S09-12: ");
  for (int i = 8; i < 12; i++) Serial.printf("%d", sensor_check[i]);
  Serial.println();
  Serial.flush();
}

// ══════════════════════════════════════════════════════════════════════
void lora_init() {
  Serial.println("[LORA] Initializing SX1278...");
  Serial.printf("[LORA] Pins: SCK=%d MISO=%d MOSI=%d NSS=%d RST=%d DIO0=%d\n",
                LORA_SCK, LORA_MISO, LORA_MOSI, LORA_NSS, LORA_RST, LORA_DIO0);
  Serial.flush();
  pinMode(LORA_RST, OUTPUT);
  digitalWrite(LORA_RST, LOW);
  delay(20);
  digitalWrite(LORA_RST, HIGH);
  delay(150);
  Serial.println("[LORA] RST pulse done");
  Serial.flush();
  lora_spi.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_NSS);
  Serial.println("[LORA] HSPI initialized");
  Serial.flush();
  LoRa.setSPI(lora_spi);
  LoRa.setPins(LORA_NSS, LORA_RST, LORA_DIO0);
  Serial.println("[LORA] Starting LoRa...");
  Serial.flush();
  for (int i = 5; i > 0; i--) {
    if (LoRa.begin(433E6)) {
      Serial.println("[LORA] Init success!");
      break;
    }
    Serial.printf("[LORA] Retry %d/5...\n", 6 - i);
    delay(500);
    if (i == 1) {
      Serial.println("[LORA] FATAL: Init FAILED!");
      Serial.println("[LORA] Check: VCC=3.3V, SPI wiring, RST pin");
      lora_enabled = false;
      return;
    }
  }
  LoRa.setSpreadingFactor(7);
  LoRa.setSignalBandwidth(125E3);
  LoRa.setCodingRate4(5);
  LoRa.setTxPower(17);
  LoRa.enableCrc();
  Serial.println("[LORA] SF:7 | BW:125kHz | CR:4/5 | 433MHz");
  Serial.flush();
  lora_enabled = true;
  if (mode_value == MODE_LORA_NODE) {
    Serial.println("[LORA] Node mode - offset delay...");
    delay(15000);
  }
  LoRa.receive();
  Serial.printf("[LORA] Listening... (Mode: %s)\n", getModeName(mode_value));
  Serial.flush();
  if (modeIsGateway(mode_value)) {
    discovery_active = true;
    discovery_start = millis();
    Serial.println("[LORA] Starting discovery phase (60s)...");
    Serial.flush();
  }
}

// ══════════════════════════════════════════════════════════════════════
bool parseLoraMessage(char* msg, char* cmd, char* payload) {
  char* start = strchr(msg, '[');
  if (start == NULL) return false;
  char* end = strchr(start, ']');
  if (end == NULL) return false;
  int cmd_len = end - start - 1;
  if (cmd_len > 15) cmd_len = 15;
  strncpy(cmd, start + 1, cmd_len);
  cmd[cmd_len] = '\0';
  if (end[1] == ' ') {
    strncpy(payload, end + 2, 63);
    payload[63] = '\0';
  } else {
    payload[0] = '\0';
  }
  return true;
}

void sendLoraMessage(const char* cmd, const char* payload) {
  LoRa.idle();
  LoRa.beginPacket();
  LoRa.print("[");
  LoRa.print(cmd);
  LoRa.print("]");
  if (payload[0] != '\0') {
    LoRa.print(" ");
    LoRa.print(payload);
  }
  bool ok = LoRa.endPacket(false);
  LoRa.receive();
  Serial.printf("[TX] [%s] %s -> %s\n", cmd, payload, ok ? "OK" : "FAILED");
  Serial.flush();
  flashLoRaLED(0, 255, 0, 100);
}

void flashLoRaLED(uint8_t r, uint8_t g, uint8_t b, unsigned long duration_ms) {
  lora_led_flash_r = r;
  lora_led_flash_g = g;
  lora_led_flash_b = b;
  lora_led_flash_end = millis() + duration_ms;
}

void updateLoRaLED() {
  unsigned long now = millis();
  if (now < lora_led_flash_end) {
    pixels.setPixelColor(0, pixels.Color(lora_led_flash_r, lora_led_flash_g, lora_led_flash_b));
    pixels.show();
    return;
  }
  
  // Pairing mode - purple blinking
  if (pairing_mode && lora_enabled) {
    if ((now / 500) % 2 == 0) {
      pixels.setPixelColor(0, pixels.Color(255, 0, 255));
    } else {
      pixels.setPixelColor(0, pixels.Color(0, 0, 0));
    }
    pixels.show();
    return;
  }
  
  if (!lora_enabled) {
    if (modeHasWiFi(mode_value)) pixels.setPixelColor(0, pixels.Color(0, 100, 0));
    else if (modeHasLAN(mode_value)) pixels.setPixelColor(0, pixels.Color(0, 0, 100));
    else pixels.setPixelColor(0, pixels.Color(255,255,255));
  } else if (modeIsGateway(mode_value)) {
    if (discovery_active) {
      if ((now / 500) % 2 == 0) {
        pixels.setPixelColor(0, pixels.Color(255, 165, 0));
      } else {
        pixels.setPixelColor(0, pixels.Color(255, 255, 0));
      }
    } else if (node_count > 0) {
      pixels.setPixelColor(0, pixels.Color(0, 0, 255));
    } else {
      pixels.setPixelColor(0, pixels.Color(0, 255, 255));
    }
  } else if (mode_value == MODE_LORA_NODE) {
    if (request_timeout > 0 && !ack_received) {
      if ((now / 200) % 2 == 0) {
        pixels.setPixelColor(0, pixels.Color(255, 0, 255));
      } else {
        pixels.setPixelColor(0, pixels.Color(0, 0, 0));
      }
    } else {
      pixels.setPixelColor(0, pixels.Color(128, 0, 128));
    }
  }
  pixels.show();
}

void addNodeToList(const char* node_id, char* assigned_id) {
  // Generate assigned ID based on node count
  snprintf(assigned_id, 8, "N%d", node_count + 1);
  
  for (int i = 0; i < node_count; i++) {
    if (strcmp(node_ids[i], assigned_id) == 0) {
      return;
    }
  }
  if (node_count < MAX_NODES) {
    strncpy(node_ids[node_count], assigned_id, 7);
    node_ids[node_count][7] = '\0';
    node_count++;
    Serial.printf("[LORA] Node discovered: %s -> Assigned ID: %s (Total: %d)\n", node_id, assigned_id, node_count);
    Serial.flush();
  }
}

// ══════════════════════════════════════════════════════════════════════
void lora_gateway_loop() {
  static unsigned long last_loop_time = 0;
  unsigned long now = millis();
  
  if (now - last_loop_time >= 5000) {
    last_loop_time = now;
    Serial.printf("[LORA] Gateway loop running, enabled=%d, discovery=%d, nodes=%d\n",
                  lora_enabled, discovery_active, node_count);
    Serial.flush();
  }

  if (!lora_enabled) return;

  if (discovery_active) {
    if (now - discovery_start >= DISCOVERY_DURATION) {
      discovery_active = false;
      Serial.printf("[LORA] Discovery complete. Found %d node(s).\n", node_count);
      Serial.printf("[LORA] Nodes: ");
      for (int i = 0; i < node_count; i++) {
        Serial.printf("%s ", node_ids[i]);
      }
      Serial.println();
      Serial.flush();
      last_request_time = now;
      current_request_node = 0;
      return;
    }

    static unsigned long last_discover_send = 0;
    if (now - last_discover_send >= DISCOVERY_INTERVAL) {
      last_discover_send = now;
      Serial.printf("[LORA] Sending DISCOVER packet...\n");
      Serial.flush();
      sendLoraMessage("DISCOVER", "");
    }

    int pktSize = LoRa.parsePacket();
    if (pktSize == 0) return;

    String raw = "";
    lora_rx_len = 0;
    while (LoRa.available()) {
      char c = LoRa.read();
      if (lora_rx_len < sizeof(lora_rx_buf) - 1) {
        lora_rx_buf[lora_rx_len++] = c;
      }
      raw += c;
    }
    lora_rx_buf[lora_rx_len] = '\0';

    int rssi = LoRa.packetRssi();
    Serial.printf("[LORA] RX: %d bytes | RSSI: %d dBm | %s\n", lora_rx_len, rssi, raw.c_str());
    flashLoRaLED(0, 255, 255, 100);

    char cmd[16], payload[64];
    if (parseLoraMessage(lora_rx_buf, cmd, payload)) {
      if (strcmp(cmd, "HELLO") == 0) {
        char assigned_id[8];
        addNodeToList(payload, assigned_id);
        // Send assigned ID back to node
        sendLoraMessage("ASSIGN", assigned_id);
      }
    }

    LoRa.receive();
    return;
  }

  if (now - last_request_time >= REQUEST_INTERVAL) {
    last_request_time = now;
    if (node_count > 0) {
      char* node_id = node_ids[current_request_node];
      Serial.printf("[LORA] Requesting data from %s\n", node_id);
      sendLoraMessage("REQUEST", node_id);
      current_request_node = (current_request_node + 1) % node_count;
    }
  }

  int pktSize = LoRa.parsePacket();
  if (pktSize == 0) return;

  String raw = "";
  lora_rx_len = 0;
  while (LoRa.available()) {
    char c = LoRa.read();
    if (lora_rx_len < sizeof(lora_rx_buf) - 1) {
      lora_rx_buf[lora_rx_len++] = c;
    }
    raw += c;
  }
  lora_rx_buf[lora_rx_len] = '\0';

  int rssi = LoRa.packetRssi();
  float snr = LoRa.packetSnr();

  Serial.printf("[LORA] RX: %d bytes | RSSI: %d dBm | SNR: %.1f dB | %s\n",
                lora_rx_len, rssi, snr, raw.c_str());
  flashLoRaLED(0, 255, 255, 100);

  char cmd[16], payload[64];
  if (parseLoraMessage(lora_rx_buf, cmd, payload)) {
    if (strcmp(cmd, "DATA") == 0) {
      memcpy(lora_last_rx_data, lora_rx_buf, lora_rx_len);
      lora_last_rx_data[lora_rx_len] = '\0';
      lora_last_rssi = rssi;
      lora_last_snr = (int8_t)snr;
      lora_last_rx_time = millis();
      lora_new_data = true;
      char* node_id = strtok(payload, "|");
      if (node_id != NULL) {
        sendLoraMessage("ACK", node_id);
      }
    }
  }

  LoRa.receive();
}

// ══════════════════════════════════════════════════════════════════════
unsigned long lora_last_tx = 0;
const unsigned long LORA_TX_INTERVAL = 30000;

void lora_node_loop() {
  if (!lora_enabled) return;

  unsigned long now = millis();

  int pktSize = LoRa.parsePacket();
  if (pktSize > 0) {
    String raw = "";
    lora_rx_len = 0;
    while (LoRa.available()) {
      char c = LoRa.read();
      if (lora_rx_len < sizeof(lora_rx_buf) - 1) {
        lora_rx_buf[lora_rx_len++] = c;
      }
      raw += c;
    }
    lora_rx_buf[lora_rx_len] = '\0';

    int rssi = LoRa.packetRssi();
    Serial.printf("[LORA] RX: %d bytes | RSSI: %d dBm | %s\n", lora_rx_len, rssi, raw.c_str());
    flashLoRaLED(0, 255, 255, 100);

    char cmd[16], payload[64];
    if (parseLoraMessage(lora_rx_buf, cmd, payload)) {
      if (strcmp(cmd, "DISCOVER") == 0) {
        // Send HELLO with temporary ID
        sendLoraMessage("HELLO", "TEMP");
        LoRa.receive();
        return;
      }

      if (strcmp(cmd, "ASSIGN") == 0) {
        // Receive and store assigned ID from gateway
        strncpy(node_id, payload, 7);
        node_id[7] = '\0';
        node_id_assigned = true;
        Serial.printf("[LORA] Assigned ID received: %s\n", node_id);
        Serial.flush();
        LoRa.receive();
        return;
      }

      if (strcmp(cmd, "REQUEST") == 0) {
        if (strcmp(payload, node_id) == 0) {
          char data_buf[64];
          snprintf(data_buf, sizeof(data_buf), "%s|%lu", node_id, millis() / 1000);
          Serial.printf("[LORA] Sending data: %s\n", data_buf);
          sendLoraMessage("DATA", data_buf);
          lora_last_tx = now;
          ack_received = false;
          retry_count = 0;
          request_timeout = now + 5000;
        }
        LoRa.receive();
        return;
      }

      if (strcmp(cmd, "ACK") == 0) {
        if (strcmp(payload, node_id) == 0) {
          ack_received = true;
          Serial.println("[LORA] ACK received - data confirmed");
          Serial.flush();
        }
        LoRa.receive();
        return;
      }
    }

    LoRa.receive();
  }

  if (request_timeout > 0 && !ack_received && now >= request_timeout) {
    if (retry_count < MAX_RETRIES) {
      retry_count++;
      Serial.printf("[LORA] ACK timeout - Retry %d/%d\n", retry_count, MAX_RETRIES);
      char data_buf[64];
      snprintf(data_buf, sizeof(data_buf), "%s|%lu", node_id, millis() / 1000);
      sendLoraMessage("DATA", data_buf);
      request_timeout = now + 5000;
    } else {
      Serial.println("[LORA] Max retries reached - giving up");
      request_timeout = 0;
      retry_count = 0;
    }
    LoRa.receive();
  }
}

// ══════════════════════════════════════════════════════════════════════
void restart_loop() {
  Serial.println("[RTC]  Initializing DS3231M...");
  Serial.flush();

  if (!rtc.begin(&Wire)) {
    Serial.println("[RTC]  ERROR: DS3231M not found! Halting.");
    Serial.flush();
    while (1) {
      pixels.setPixelColor(0, pixels.Color(255, 0, 0)); pixels.show(); delay(300);
      pixels.setPixelColor(0, pixels.Color(0, 0, 0));   pixels.show(); delay(300);
    }
  }
  Serial.println("[RTC]  DS3231M found!");
  Serial.flush();

  if (rtc.lostPower()) {
    Serial.println("[RTC]  Lost power — setting compile time");
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }

  rtc.writeSqwPinMode(DS3231_OFF);
  writeReg(REG_CTRL, readReg(REG_CTRL) | 0x05);

  unsigned long cycleCount = 0;

  while (true) {
    cycleCount++;
    DateTime now = rtc.now();

    Serial.println("========================================");
    Serial.printf("[SYS]  Cycle #%lu started\n", cycleCount);
    Serial.printf("[RTC]  %02d:%02d:%02d\n", now.hour(), now.minute(), now.second());
    Serial.printf("[CFG]  ON=%luh  OFF=%lus\n", ON_SECONDS / 3600, OFF_SECONDS);
    Serial.println("========================================");
    Serial.flush();

    writeReg(REG_STATUS, readReg(REG_STATUS) | 0x01);
    pixels.setPixelColor(0, pixels.Color(0, 255, 0));
    pixels.show();

    Serial.printf("[RELAY] ON — counting %luh\n", ON_SECONDS / 3600);
    Serial.flush();

    unsigned long last_sec_tick = millis();
    for (unsigned long remaining = ON_SECONDS; remaining > 0; ) {
      // Check buttons (GPIO 45 + 46)
      checkButtons();
      
      // Handle web servers (portal or LAN config)
      handlePortal();
      handleLANServer();
      
      // LoRa processing runs every iteration — no blocking delay
      if (lora_enabled) {
        if (pairing_mode) {
          if (modeIsGateway(mode_value)) lora_pairing_mode_gateway();
          else if (mode_value == MODE_LORA_NODE) lora_pairing_mode_node();
        } else {
          if (modeIsGateway(mode_value)) lora_gateway_loop();
          else if (mode_value == MODE_LORA_NODE) lora_node_loop();
        }
      }
      updateLoRaLED();

      // 1-second tick: status print + countdown
      unsigned long now_ms = millis();
      if (now_ms - last_sec_tick >= 1000) {
        last_sec_tick = now_ms;
        remaining--;
        esp_task_wdt_reset();

        if (remaining % 60 == 0 || remaining <= 10) {
          DateTime t = rtc.now();
          unsigned long h = remaining / 3600;
          unsigned long m = (remaining % 3600) / 60;
          unsigned long s = remaining % 60;
          Serial.printf("[ON]  %02d:%02d:%02d | Relay=ON | %luh %lum %lus left | %s\n",
                        t.hour(), t.minute(), t.second(), h, m, s, i2cAddrString().c_str());
          mode_check();
          sensor_check_func();
          read_power();
          onboard_temp_hum();

          if (lora_enabled) {
            if (modeIsGateway(mode_value)) {
              if (discovery_active) {
                Serial.println("[LORA] Gateway: DISCOVERY phase active");
              } else if (node_count > 0) {
                Serial.printf("[LORA] Gateway: CONNECTED (%d nodes)\n", node_count);
                if (lora_last_rx_time > 0) {
                  unsigned long seconds_ago = (millis() - lora_last_rx_time) / 1000;
                  Serial.printf("[LORA] Last RX (%lu sec ago): RSSI=%d dBm SNR=%d dB | %s\n",
                                seconds_ago, lora_last_rssi, lora_last_snr, lora_last_rx_data);
                }
              } else {
                Serial.println("[LORA] Gateway: Waiting for nodes...");
              }
            } else if (mode_value == MODE_LORA_NODE) {
              unsigned long tx_age = millis() - lora_last_tx;
              if (request_timeout > 0 && !ack_received) {
                Serial.printf("[LORA] Node: Waiting for ACK (retry %d/3)\n", retry_count);
              } else if (ack_received) {
                Serial.printf("[LORA] Node: Data confirmed (last TX %lu sec ago)\n", tx_age / 1000);
              } else {
                Serial.printf("[LORA] Node: Listening (last TX %lu sec ago)\n", tx_age / 1000);
              }
            }
          } else {
            Serial.println("[LORA] Not enabled (check mode or init status)");
          }

          Serial.println("----------------------------------------");
        }
      }
    }

    writeReg(REG_STATUS, readReg(REG_STATUS) & ~0x01);
    pixels.setPixelColor(0, pixels.Color(255, 0, 0));
    pixels.show();

    Serial.printf("[RELAY] OFF — waiting %lus\n", OFF_SECONDS);
    Serial.flush();

    for (unsigned long i = 0; i < OFF_SECONDS; i++) {
      delay(1000);
      esp_task_wdt_reset();
    }
  }
}

// ══════════════════════════════════════════════════════════════════════
void setup() {
  pixels.begin();
  pixels.setBrightness(50);
  pixels.setPixelColor(0, pixels.Color(255, 255, 255));
  pixels.show();

  Serial.begin(115200);
  delay(5000);

  Serial.println();
  Serial.println("========================================");
  Serial.println("[SYS]  ESP32-S3 Booting...");
  Serial.printf("[SYS]  SDK: %s\n", ESP.getSdkVersion());
  Serial.printf("[SYS]  Free heap: %lu bytes\n", (unsigned long)ESP.getFreeHeap());
  Serial.println("========================================");
  Serial.flush();

  pixels.setPixelColor(0, pixels.Color(128, 0, 128));
  pixels.show();

  Serial.println("[I2C]  Bus recovery...");
  i2cBusRecovery();

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setTimeOut(200);
  Serial.println("[I2C]  Wire initialized");
  Serial.flush();

  pixels.setPixelColor(0, pixels.Color(0, 0, 255));
  pixels.show();

  pinMode(INT_PIN, INPUT_PULLUP);
  pinMode(BTN_STATUS_PIN, INPUT_PULLUP);
  pinMode(BTN_PAIRING_PIN, INPUT_PULLUP);

  esp_task_wdt_init(30, true);
  esp_task_wdt_add(NULL);
  Serial.println("[WDT]  Watchdog enabled (30s)");
  Serial.println("[BTN]  GPIO45=Status/Reset, GPIO46=Pairing/Portal");
  Serial.flush();
  
  // Load network config from NVS
  loadNetworkConfig(netConfig);
  Serial.printf("[CFG]  Mode name: %s\n", getModeName(mode_value));
  Serial.printf("[CFG]  IP mode: %s\n", netConfig.ip_mode ? "Static" : "DHCP");
  if (netConfig.wifi_ssid[0]) Serial.printf("[CFG]  WiFi SSID: %s\n", netConfig.wifi_ssid);
  if (netConfig.tb_server[0]) Serial.printf("[CFG]  TB Server: %s\n", netConfig.tb_server);
  Serial.flush();

  mcp23017_2_init();
  sensor_en_set(16, true);
  Serial.println("[MCP23] sensor_en16 = ON (before I2C scan)");
  Serial.flush();

  checkI2C();
  verifyI2CDevices();
  mcp23017_1_init();
  mcp23017_3_init();
  led_sys(true);
  mode_check();

  Serial.printf("[SYS]  Mode = %u (%s)\n", mode_value, getModeName(mode_value));
  Serial.flush();
  
  // Initialize based on mode capabilities
  if (modeHasLAN(mode_value)) {
    initLAN();
  }
  if (modeHasWiFi(mode_value)) {
    initWifi();
  }
  if (modeHasLoRa(mode_value)) {
    lora_init();
  }
  if (!modeHasLoRa(mode_value)) {
    lora_enabled = false;
  }
  Serial.flush();

  restart_loop();
}

void loop() {
}