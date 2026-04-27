// ── Captive Portal for WiFi Configuration ──────────────────────────────
#ifndef CAPTIVE_PORTAL_H
#define CAPTIVE_PORTAL_H

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include "eth_config.h"

extern WebServer portalServer;
extern bool portal_active;
extern bool ap_mode_active;

static DNSServer dnsServer;

static const char PORTAL_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html><head>
<meta charset='UTF-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>Device Config</title>
<style>
body{font-family:sans-serif;margin:20px;background:#1a1a2e;color:#e0e0e0}
h1{color:#00d4ff;text-align:center}
.card{background:#16213e;border-radius:10px;padding:20px;margin:10px 0}
input{width:100%;padding:10px;margin:5px 0;border:1px solid #0f3460;border-radius:5px;box-sizing:border-box;background:#1a1a2e;color:#e0e0e0}
button{background:#00d4ff;color:#000;border:none;padding:12px 30px;border-radius:5px;cursor:pointer;font-size:16px;width:100%;margin-top:10px}
button:hover{background:#00a8cc}
label{display:block;margin-top:10px;color:#00d4ff;font-weight:bold}
.section{border-left:3px solid #00d4ff;padding-left:15px;margin:15px 0}
</style>
</head><body>
<h1>&#9881; Device Configuration</h1>
<form action='/save' method='POST'>
<div class='card'>
<div class='section'>
<h3>WiFi Settings</h3>
<label>SSID</label><input name='ssid' placeholder='WiFi SSID'>
<label>Password</label><input name='pass' type='password' placeholder='WiFi Password'>
</div>
<div class='section'>
<h3>ThingsBoard Server</h3>
<label>Server URL</label><input name='tb_server' placeholder='https://thingsboard.example.com'>
<label>Access Token</label><input name='tb_token' placeholder='Device Token'>
</div>
<div class='section'>
<h3>IP Configuration</h3>
<label><input type='radio' name='ip_mode' value='0' checked> DHCP</label>
<label><input type='radio' name='ip_mode' value='1'> Static IP</label>
<label>Static IP</label><input name='ip' placeholder='192.168.1.100'>
<label>Subnet</label><input name='subnet' placeholder='255.255.255.0'>
<label>Gateway</label><input name='gw' placeholder='192.168.1.1'>
<label>DNS</label><input name='dns' placeholder='8.8.8.8'>
</div>
</div>
<button type='submit'>Save &amp; Restart</button>
</form>
</body></html>
)rawliteral";

inline void startCaptivePortal() {
  Serial.println("[PORTAL] Starting Captive Portal AP...");
  WiFi.mode(WIFI_AP);
  WiFi.softAP("ESP32-Setup", "");
  ap_mode_active = true;
  portal_active = true;
  dnsServer.start(53, "*", WiFi.softAPIP());
  portalServer.on("/", HTTP_GET, []() {
    portalServer.sendHeader("Connection", "close");
    portalServer.send(200, "text/html", PORTAL_HTML);
  });
  portalServer.on("/save", HTTP_POST, []() {
    String ssid = portalServer.arg("ssid");
    String pass = portalServer.arg("pass");
    String tb_server = portalServer.arg("tb_server");
    String tb_token = portalServer.arg("tb_token");
    String ip_mode_str = portalServer.arg("ip_mode");
    strncpy(netConfig.wifi_ssid, ssid.c_str(), sizeof(netConfig.wifi_ssid) - 1);
    strncpy(netConfig.wifi_pass, pass.c_str(), sizeof(netConfig.wifi_pass) - 1);
    strncpy(netConfig.tb_server, tb_server.c_str(), sizeof(netConfig.tb_server) - 1);
    strncpy(netConfig.tb_token, tb_token.c_str(), sizeof(netConfig.tb_token) - 1);
    netConfig.ip_mode = (uint8_t)ip_mode_str.toInt();
    if (netConfig.ip_mode == 1) {
      String ip = portalServer.arg("ip");
      String subnet = portalServer.arg("subnet");
      String gw = portalServer.arg("gw");
      String dns = portalServer.arg("dns");
      strncpy(netConfig.static_ip, ip.c_str(), sizeof(netConfig.static_ip) - 1);
      strncpy(netConfig.subnet, subnet.c_str(), sizeof(netConfig.subnet) - 1);
      strncpy(netConfig.gateway, gw.c_str(), sizeof(netConfig.gateway) - 1);
      strncpy(netConfig.dns, dns.c_str(), sizeof(netConfig.dns) - 1);
    }
    saveNetworkConfig(netConfig);
    portalServer.sendHeader("Connection", "close");
    portalServer.send(200, "text/html", "<h1>Saved! Restarting...</h1>");
    delay(1000);
    ESP.restart();
  });
  portalServer.onNotFound([]() {
    portalServer.sendHeader("Connection", "close");
    portalServer.send(200, "text/html", PORTAL_HTML);
  });
  portalServer.begin();
  Serial.printf("[PORTAL] AP IP: %s\n", WiFi.softAPIP().toString().c_str());
  Serial.flush();
}

inline void handlePortal() {
  if (portal_active) {
    dnsServer.processNextRequest();
    portalServer.handleClient();
  }
}

#endif // CAPTIVE_PORTAL_H