// Button handler — GPIO 45 (status) + GPIO 46 (pairing/portal)
//
// GPIO 45 short press      → showAllStatuses() (1-2-3-4-5 blink groups)
// GPIO 45 hold ≥ 10s       → factory reset (clear NVS) + reboot
// GPIO 46 hold ≥  5s, <10s → toggle LoRa pairing mode
// GPIO 46 hold ≥ 10s       → captive portal (WiFi) / LAN web config
//
// Call buttonsBegin() once in setup(), then buttonsLoop() every iteration.
#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include <Adafruit_NeoPixel.h>
#include "eth_config.h"
#include "captive_portal.h"
#include "lan_webconfig.h"
#include "lora_gateway.h"
#include "status_display.h"

#define BTN_STATUS_GPIO        45
#define BTN_PAIRING_GPIO       46
#define BTN_PAIRING_HOLD_MS    5000UL    // 5s for LoRa pairing
#define BTN_PORTAL_HOLD_MS     10000UL   // 10s for portal / factory reset

extern Adafruit_NeoPixel pixels;
extern bool wifi_connected;
extern bool lan_connected;

static bool      _btn45Held       = false;
static uint32_t  _btn45Start      = 0;
static bool      _btn46Held       = false;
static uint32_t  _btn46Start      = 0;

inline void buttonsBegin() {
  pinMode(BTN_STATUS_GPIO,  INPUT_PULLUP);
  pinMode(BTN_PAIRING_GPIO, INPUT_PULLUP);
}

static void _factoryReset() {
  Serial.println("[BTN45] FACTORY RESET — clearing NVS");
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, false);
  prefs.clear();
  prefs.end();
  for (int i = 0; i < 5; i++) {
    pixels.setPixelColor(0, pixels.Color(255, 0, 0)); pixels.show(); delay(150);
    pixels.setPixelColor(0, 0); pixels.show(); delay(150);
  }
  ESP.restart();
}

static void _enterConfigPortal() {
  Serial.println("[BTN46] CONFIG PORTAL");
  // ถ้ามี WiFi mode active ให้เปิด captive portal, ไม่งั้นเปิด LAN web config
  if (wifi_connected || !lan_connected) startCaptivePortal();
  else                                   startLANWebServer();
}

inline void buttonsLoop() {
  uint32_t now = millis();

  // ── GPIO 45: status / factory reset ──────────────────────────────────
  int b45 = digitalRead(BTN_STATUS_GPIO);
  if (b45 == LOW && !_btn45Held) {
    _btn45Held = true;
    _btn45Start = now;
  }
  if (b45 == HIGH && _btn45Held) {
    _btn45Held = false;
    uint32_t dur = now - _btn45Start;
    if (dur >= BTN_PORTAL_HOLD_MS)        _factoryReset();
    else if (dur >= 50)                   showAllStatuses();   // debounce
  }
  // Visual hint while holding GPIO 45 past factory-reset threshold
  if (b45 == LOW && _btn45Held && (now - _btn45Start) >= BTN_PORTAL_HOLD_MS) {
    pixels.setPixelColor(0, ((now / 100) % 2) ? pixels.Color(255, 0, 0) : 0);
    pixels.show();
  }

  // ── GPIO 46: pairing / portal ────────────────────────────────────────
  int b46 = digitalRead(BTN_PAIRING_GPIO);
  if (b46 == LOW && !_btn46Held) {
    _btn46Held = true;
    _btn46Start = now;
  }
  if (b46 == HIGH && _btn46Held) {
    _btn46Held = false;
    uint32_t dur = now - _btn46Start;
    if (dur >= BTN_PORTAL_HOLD_MS) {
      _enterConfigPortal();
    } else if (dur >= BTN_PAIRING_HOLD_MS) {
      loraPairingToggle();
      // LED feedback
      uint32_t col = loraPairingActive() ? pixels.Color(180, 0, 180)
                                         : pixels.Color(0, 0, 80);
      for (int i = 0; i < 3; i++) {
        pixels.setPixelColor(0, col); pixels.show(); delay(120);
        pixels.setPixelColor(0, 0);   pixels.show(); delay(120);
      }
    }
  }
  // Visual hint while holding GPIO 46
  if (b46 == LOW && _btn46Held) {
    uint32_t held = now - _btn46Start;
    if (held >= BTN_PORTAL_HOLD_MS) {
      pixels.setPixelColor(0, ((now / 100) % 2) ? pixels.Color(255, 165, 0) : 0);
      pixels.show();
    } else if (held >= BTN_PAIRING_HOLD_MS) {
      pixels.setPixelColor(0, ((now / 100) % 2) ? pixels.Color(180, 0, 180) : 0);
      pixels.show();
    }
  }

  // ── While in pairing mode: gentle purple breathing ───────────────────
  if (loraPairingActive() && !_btn46Held && !_btn45Held) {
    static uint32_t lastBlink = 0;
    if (now - lastBlink >= 600) {
      lastBlink = now;
      static bool on = false;
      on = !on;
      pixels.setPixelColor(0, on ? pixels.Color(120, 0, 120) : 0);
      pixels.show();
    }
  }
}
