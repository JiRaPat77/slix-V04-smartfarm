// ── W5500 Ethernet Configuration ──────────────────────────────────────
// ESP32-S3 FSPI pins for Waveshare W5500 board
// GPIO11=ETH_MOSI, GPIO12=ETH_MISO, GPIO13=ETH_CLK, GPIO14=ETH_CS
// GPIO10=ETH_INT, GPIO9=ETH_RST

#ifndef ETH_CONFIG_H
#define ETH_CONFIG_H

#include <Arduino.h>
#include <SPI.h>
#include <Preferences.h>

// ── W5500 SPI Pin Definitions (FSPI) ────────────────────────────────
#define ETH_MOSI    11
#define ETH_MISO    12
#define ETH_SCLK    13
#define ETH_CS      14
#define ETH_INT     10
#define ETH_RST     9

// ── NVS Keys ─────────────────────────────────────────────────────────
#define NVS_NAMESPACE       "netconfig"
#define NVS_WIFI_SSID       "wifi_ssid"
#define NVS_WIFI_PASS       "wifi_pass"
#define NVS_TB_SERVER       "tb_server"
#define NVS_TB_TOKEN        "tb_token"
#define NVS_IP_MODE         "ip_mode"
#define NVS_STATIC_IP       "static_ip"
#define NVS_SUBNET          "subnet"
#define NVS_GATEWAY         "gateway"
#define NVS_DNS             "dns"
#define NVS_LORA_PAIRED     "lora_paired"
#define NVS_NODE_ID         "node_id"

// ── Mode Value Table ─────────────────────────────────────────────────
#define MODE_LAN_LORA_GW    1
#define MODE_LAN_STANDALONE 3
#define MODE_WIFI_LORA_GW   5
#define MODE_LORA_NODE      6
#define MODE_WIFI_STANDALONE 7

// ── Mode Helper Functions ────────────────────────────────────────────
inline bool modeHasWiFi(uint8_t mode) {
  return (mode == MODE_WIFI_LORA_GW || mode == MODE_WIFI_STANDALONE);
}

inline bool modeHasLAN(uint8_t mode) {
  return (mode == MODE_LAN_LORA_GW || mode == MODE_LAN_STANDALONE);
}

inline bool modeHasLoRa(uint8_t mode) {
  return (mode == MODE_LAN_LORA_GW || mode == MODE_WIFI_LORA_GW || mode == MODE_LORA_NODE);
}

inline bool modeIsGateway(uint8_t mode) {
  return (mode == MODE_LAN_LORA_GW || mode == MODE_WIFI_LORA_GW);
}

inline const char* getModeName(uint8_t mode) {
  switch (mode) {
    case MODE_LAN_LORA_GW:     return "LAN+LoRa Gateway";
    case MODE_LAN_STANDALONE:  return "LAN Standalone";
    case MODE_WIFI_LORA_GW:    return "WiFi+LoRa Gateway";
    case MODE_LORA_NODE:       return "LoRa Node";
    case MODE_WIFI_STANDALONE: return "WiFi Standalone";
    default:                   return "Unknown";
  }
}

// ── Network Config Struct ────────────────────────────────────────────
struct NetworkConfig {
  char wifi_ssid[64];
  char wifi_pass[64];
  char tb_server[128];
  char tb_token[64];
  uint8_t ip_mode;
  char static_ip[16];
  char subnet[16];
  char gateway[16];
  char dns[16];
  bool lora_paired;
  char node_id[8];
};

// ── Default Config ───────────────────────────────────────────────────
inline NetworkConfig getDefaultConfig() {
  NetworkConfig cfg = {};
  memset(cfg.wifi_ssid, 0, sizeof(cfg.wifi_ssid));
  memset(cfg.wifi_pass, 0, sizeof(cfg.wifi_pass));
  memset(cfg.tb_server, 0, sizeof(cfg.tb_server));
  memset(cfg.tb_token, 0, sizeof(cfg.tb_token));
  cfg.ip_mode = 0;
  strcpy(cfg.static_ip, "192.168.1.100");
  strcpy(cfg.subnet, "255.255.255.0");
  strcpy(cfg.gateway, "192.168.1.1");
  strcpy(cfg.dns, "8.8.8.8");
  cfg.lora_paired = false;
  strcpy(cfg.node_id, "N0");
  return cfg;
}

// ── Preferences Load/Save ────────────────────────────────────────────
inline void loadNetworkConfig(NetworkConfig &cfg) {
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, true);
  memset(&cfg, 0, sizeof(cfg));
  cfg.ip_mode = 0;
  strcpy(cfg.static_ip, "192.168.1.100");
  strcpy(cfg.subnet, "255.255.255.0");
  strcpy(cfg.gateway, "192.168.1.1");
  strcpy(cfg.dns, "8.8.8.8");
  strcpy(cfg.node_id, "N0");
  prefs.getString(NVS_WIFI_SSID, cfg.wifi_ssid, sizeof(cfg.wifi_ssid));
  prefs.getString(NVS_WIFI_PASS, cfg.wifi_pass, sizeof(cfg.wifi_pass));
  prefs.getString(NVS_TB_SERVER, cfg.tb_server, sizeof(cfg.tb_server));
  prefs.getString(NVS_TB_TOKEN, cfg.tb_token, sizeof(cfg.tb_token));
  cfg.ip_mode = prefs.getUChar(NVS_IP_MODE, 0);
  prefs.getString(NVS_STATIC_IP, cfg.static_ip, sizeof(cfg.static_ip));
  prefs.getString(NVS_SUBNET, cfg.subnet, sizeof(cfg.subnet));
  prefs.getString(NVS_GATEWAY, cfg.gateway, sizeof(cfg.gateway));
  prefs.getString(NVS_DNS, cfg.dns, sizeof(cfg.dns));
  cfg.lora_paired = prefs.getBool(NVS_LORA_PAIRED, false);
  prefs.getString(NVS_NODE_ID, cfg.node_id, sizeof(cfg.node_id));
  prefs.end();
}

inline void saveNetworkConfig(const NetworkConfig &cfg) {
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, false);
  if (cfg.wifi_ssid[0]) prefs.putString(NVS_WIFI_SSID, cfg.wifi_ssid);
  if (cfg.wifi_pass[0]) prefs.putString(NVS_WIFI_PASS, cfg.wifi_pass);
  if (cfg.tb_server[0]) prefs.putString(NVS_TB_SERVER, cfg.tb_server);
  if (cfg.tb_token[0]) prefs.putString(NVS_TB_TOKEN, cfg.tb_token);
  prefs.putUChar(NVS_IP_MODE, cfg.ip_mode);
  prefs.putString(NVS_STATIC_IP, cfg.static_ip);
  prefs.putString(NVS_SUBNET, cfg.subnet);
  prefs.putString(NVS_GATEWAY, cfg.gateway);
  prefs.putString(NVS_DNS, cfg.dns);
  prefs.putBool(NVS_LORA_PAIRED, cfg.lora_paired);
  prefs.putString(NVS_NODE_ID, cfg.node_id);
  prefs.end();
}

// ── FSPI Instance for W5500 ──────────────────────────────────────────
extern SPIClass eth_spi;

// ── Global Network Config ────────────────────────────────────────────
extern NetworkConfig netConfig;
extern bool ap_mode_active;

#endif // ETH_CONFIG_H