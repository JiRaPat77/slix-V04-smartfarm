// Status Display — short button press triggers a sequence of LED blink groups.
// Each group is N blinks where N indicates which subsystem is being checked,
// then the group's colour signals OK/WARN/FAIL.
//
// Sequence (in order):
//   1 blink  → Internet (LAN/WiFi)
//   2 blinks → I2C bus  (DS3231, MCP23×3, DHT20)
//   3 blinks → LoRa     (paired node count)
//   4 blinks → MQTT + offline queue
//   5 blinks → SD card
//
// Colour code per group: green=OK, yellow=warn, red=fail
#pragma once
#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_NeoPixel.h>
#include "mqtt_tb.h"
#include "offline_log.h"
#include "sd_card.h"
#include "lora_protocol.h"
#include "lora_registry.h"

extern Adafruit_NeoPixel pixels;
extern bool wifi_connected;
extern bool lan_connected;

#define STATUS_BLINK_ON_MS   220
#define STATUS_BLINK_OFF_MS  220
#define STATUS_GROUP_GAP_MS  900

static void _blinkGroup(int count, uint8_t r, uint8_t g, uint8_t b) {
  for (int i = 0; i < count; i++) {
    pixels.setPixelColor(0, pixels.Color(r, g, b)); pixels.show();
    delay(STATUS_BLINK_ON_MS);
    pixels.setPixelColor(0, 0); pixels.show();
    delay(STATUS_BLINK_OFF_MS);
  }
  delay(STATUS_GROUP_GAP_MS);
}

static bool _i2cAlive(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission(50) == 0;
}

inline void showAllStatuses() {
  Serial.println("\n========== SYSTEM STATUS ==========");

  // 1) Internet ----------------------------------------------------------
  bool inet = wifi_connected || lan_connected;
  Serial.printf("[STATUS] (1) Internet : %s%s%s\n",
                lan_connected ? "LAN " : "",
                wifi_connected ? "WiFi " : "",
                inet ? "OK" : "DOWN");
  _blinkGroup(1, inet ? 0 : 255, inet ? 200 : 0, 0);

  // 2) I2C ---------------------------------------------------------------
  int i2cFound = 0;
  if (_i2cAlive(0x68)) i2cFound++;   // DS3231
  if (_i2cAlive(0x27)) i2cFound++;   // MCP23017 #1
  if (_i2cAlive(0x20)) i2cFound++;   // MCP23017 #2
  bool dht_ok = _i2cAlive(0x38);
  if (dht_ok) i2cFound++;
  bool i2c_ok = (i2cFound >= 3);  // 3 required: RTC + MCP1 + MCP2
  Serial.printf("[STATUS] (2) I2C      : %d device(s) (%s)\n",
                i2cFound, i2c_ok ? "OK" : "FAIL");
  _blinkGroup(2, i2c_ok ? 0 : 255, i2c_ok ? 200 : 0, 0);

  // 3) LoRa --------------------------------------------------------------
  bool lora_ok = loraAvailable();
  int  paired  = loraRegistryCount();
  Serial.printf("[STATUS] (3) LoRa     : module %s, paired=%d\n",
                lora_ok ? "OK" : "FAIL", paired);
  if (!lora_ok)         _blinkGroup(3, 255, 0, 0);
  else if (paired == 0) _blinkGroup(3, 255, 200, 0);   // yellow = no nodes yet
  else                  _blinkGroup(3, 0, 200, 0);

  // 4) MQTT + Offline queue ---------------------------------------------
  bool mqtt_ok = mqttIsConnected();
  uint32_t qlen = offlinePendingCount();
  Serial.printf("[STATUS] (4) MQTT     : %s, offline_queue=%u\n",
                mqtt_ok ? "Connected" : "Disconnected", qlen);
  if (!mqtt_ok)        _blinkGroup(4, 255, 0, 0);
  else if (qlen > 0)   _blinkGroup(4, 255, 200, 0);    // yellow = catching up
  else                 _blinkGroup(4, 0, 200, 0);

  // 5) SD card -----------------------------------------------------------
  bool sd_ok = sdAvailable();
  Serial.printf("[STATUS] (5) SD card  : %s\n", sd_ok ? "Mounted" : "Not available");
  _blinkGroup(5, sd_ok ? 0 : 255, sd_ok ? 200 : 200, 0);

  Serial.println("===================================\n");
  pixels.setPixelColor(0, pixels.Color(0, 0, 60)); pixels.show(); // back to idle blue
}
