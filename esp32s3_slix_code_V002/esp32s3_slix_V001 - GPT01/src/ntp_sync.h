// NTP Time Sync — รองรับทั้ง WiFi (configTime) และ LAN/W5500 (UDP port 123)
// หลัง sync แล้ว RTC จะเก็บเวลา UTC → ไม่ต้อง offset ก่อนส่ง ThingsBoard
#pragma once
#include <Arduino.h>
#include <RTClib.h>
#include <WiFi.h>
#include <Ethernet.h>
#include <EthernetUdp.h>
#include <time.h>

extern RTC_DS3231 rtc;
extern bool wifi_connected;
extern bool lan_connected;

#define NTP_SERVER_PRIMARY    "pool.ntp.org"
#define NTP_SERVER_FALLBACK   "time.google.com"
#define NTP_LOCAL_PORT        2390
#define NTP_PACKET_SIZE       48

static bool _ntpSynced = false;

inline bool ntpIsSynced() { return _ntpSynced; }

// ── WiFi NTP via configTime/getLocalTime ──────────────────────────────────
static bool _ntpSyncWiFi() {
  configTime(0, 0, NTP_SERVER_PRIMARY, NTP_SERVER_FALLBACK);
  struct tm ti = {};
  for (int i = 0; i < 20; i++) {
    if (getLocalTime(&ti, 500)) {
      if (ti.tm_year + 1900 < 2024) { delay(300); continue; } // skip default 1970
      DateTime utc(ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
                   ti.tm_hour, ti.tm_min, ti.tm_sec);
      rtc.adjust(utc);
      Serial.printf("[NTP] WiFi sync OK → %04d-%02d-%02d %02d:%02d:%02d UTC\n",
                    utc.year(), utc.month(), utc.day(),
                    utc.hour(), utc.minute(), utc.second());
      return true;
    }
    delay(300);
  }
  return false;
}

// ── LAN NTP via EthernetUDP (raw NTPv4 client) ───────────────────────────
static bool _ntpSyncLAN() {
  EthernetUDP udp;
  if (!udp.begin(NTP_LOCAL_PORT)) {
    Serial.println("[NTP] UDP begin failed");
    return false;
  }

  uint8_t pkt[NTP_PACKET_SIZE] = {};
  pkt[0] = 0xE3;  // LI=3 (unsync), VN=4, Mode=3 (client)
  pkt[1] = 0;     // stratum
  pkt[2] = 6;     // poll interval
  pkt[3] = 0xEC;  // precision

  const char* servers[] = { NTP_SERVER_PRIMARY, NTP_SERVER_FALLBACK };
  for (int s = 0; s < 2; s++) {
    if (!udp.beginPacket(servers[s], 123)) continue;
    udp.write(pkt, NTP_PACKET_SIZE);
    udp.endPacket();

    uint32_t t0 = millis();
    while (millis() - t0 < 2500) {
      if (udp.parsePacket() >= NTP_PACKET_SIZE) {
        udp.read(pkt, NTP_PACKET_SIZE);
        uint32_t secs = ((uint32_t)pkt[40] << 24) | ((uint32_t)pkt[41] << 16) |
                        ((uint32_t)pkt[42] << 8)  |  (uint32_t)pkt[43];
        if (secs > 2208988800UL) {
          uint32_t unix = secs - 2208988800UL;  // NTP epoch → Unix epoch
          rtc.adjust(DateTime(unix));
          DateTime t = rtc.now();
          Serial.printf("[NTP] LAN sync OK (%s) → %04d-%02d-%02d %02d:%02d:%02d UTC\n",
                        servers[s],
                        t.year(), t.month(), t.day(),
                        t.hour(), t.minute(), t.second());
          udp.stop();
          return true;
        }
      }
      delay(20);
    }
  }
  udp.stop();
  return false;
}

// เรียกเมื่อมี internet พร้อม → sync RTC ครั้งเดียว (จะ retry ทุกๆ 1 ชม.)
inline bool ntpSyncOnce() {
  static uint32_t _lastTry = 0;
  uint32_t now = millis();
  if (_ntpSynced && (now - _lastTry < 3600000UL)) return true;
  if (now - _lastTry < 30000UL) return _ntpSynced;  // retry ทุก 30s
  _lastTry = now;

  Serial.println("[NTP] Syncing time...");
  bool ok = false;
  if (wifi_connected)      ok = _ntpSyncWiFi();
  else if (lan_connected)  ok = _ntpSyncLAN();
  else {
    Serial.println("[NTP] No network");
    return false;
  }

  if (ok) _ntpSynced = true;
  else    Serial.println("[NTP] Sync FAILED");
  return ok;
}
