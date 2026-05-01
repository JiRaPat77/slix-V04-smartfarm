// ThingsBoard MQTT Client
// Connects via LAN (W5500) or WiFi, publishes to v1/gateway/telemetry
#pragma once
#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <Ethernet.h>

#define TB_DEVICE_TOKEN     "gWAOCeC6SkmzsceIGa8Q"
#define TB_TOPIC_TEL        "v1/gateway/telemetry"
#define TB_TOPIC_DEVICE_TEL "v1/devices/me/telemetry"
#define TB_PORT             1883
#define TB_CLIENT_ID     "SLXA1260004_GW"
#define MQTT_BUF_SIZE    8192
#define MQTT_RETRY_MS    15000

extern bool wifi_connected;
extern bool lan_connected;

static WiFiClient     _mqttWifiClient;
static EthernetClient _mqttLanClient;
static PubSubClient   _mqtt;
static char           _mqttServer[128] = {};
static unsigned long  _mqttLastTry     = 0;
static bool           _mqttUseLAN      = false;

// Call once with the ThingsBoard server hostname/IP from netConfig
inline void mqttSetup(const char* server) {
  strlcpy(_mqttServer, server, sizeof(_mqttServer));
  _mqtt.setBufferSize(MQTT_BUF_SIZE);
}

// (Re)connect — call from network task; returns true if connected
inline bool mqttConnect() {
  if (_mqtt.connected()) return true;

  unsigned long now = millis();
  if (now - _mqttLastTry < MQTT_RETRY_MS) return false;
  _mqttLastTry = now;

  if (_mqttServer[0] == '\0') return false;

  // Prefer LAN for stability
  if (lan_connected) {
    _mqtt.setClient(_mqttLanClient);
    _mqttUseLAN = true;
  } else if (wifi_connected) {
    _mqtt.setClient(_mqttWifiClient);
    _mqttUseLAN = false;
  } else {
    return false;
  }

  _mqtt.setServer(_mqttServer, TB_PORT);
  Serial.printf("[MQTT] Connecting to %s (%s)...\n",
                _mqttServer, _mqttUseLAN ? "LAN" : "WiFi");

  bool ok = _mqtt.connect(TB_CLIENT_ID, TB_DEVICE_TOKEN, nullptr);
  if (ok) Serial.println("[MQTT] Connected");
  else    Serial.printf("[MQTT] Failed, state=%d\n", _mqtt.state());
  return ok;
}

// Must be called regularly to keep connection alive
inline void mqttLoop() { _mqtt.loop(); }

inline bool mqttIsConnected() { return _mqtt.connected(); }

// Publish a JSON string to the gateway telemetry topic (v1/gateway/telemetry)
inline bool mqttPublish(const char* json_payload, size_t len = 0) {
  if (!_mqtt.connected()) return false;
  if (len == 0) len = strlen(json_payload);
  bool ok = _mqtt.publish(TB_TOPIC_TEL,
                           (const uint8_t*)json_payload, len, false);
  if (!ok) Serial.printf("[MQTT] Publish failed (state=%d)\n", _mqtt.state());
  return ok;
}

// Publish to device telemetry topic (v1/devices/me/telemetry) — uses same token
inline bool mqttPublishDevice(const char* json_payload, size_t len = 0) {
  if (!_mqtt.connected()) return false;
  if (len == 0) len = strlen(json_payload);
  bool ok = _mqtt.publish(TB_TOPIC_DEVICE_TEL,
                           (const uint8_t*)json_payload, len, false);
  if (!ok) Serial.printf("[MQTT] Publish (device) failed (state=%d)\n", _mqtt.state());
  return ok;
}
