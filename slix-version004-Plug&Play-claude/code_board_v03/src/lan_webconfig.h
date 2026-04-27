// ── LAN Web Configuration for W5500 ──────────────────────────────────
#ifndef LAN_WEBCONFIG_H
#define LAN_WEBCONFIG_H

#include <Arduino.h>
#include <Ethernet.h>
#include <WebServer.h>
#include "eth_config.h"

extern WebServer lanServer;
extern bool lan_server_active;

static const char LAN_CONFIG_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html><head>
<meta charset='UTF-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>LAN Device Config</title>
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
<h1>&#9881; LAN Device Configuration</h1>
<form action='/save' method='POST'>
<div class='card'>
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

inline void startLANWebServer() {
  if (lan_server_active) return;
  lanServer.on("/", HTTP_GET, []() {
    lanServer.sendHeader("Connection", "close");
    lanServer.send(200, "text/html", LAN_CONFIG_HTML);
  });
  lanServer.on("/save", HTTP_POST, []() {
    String tb_server = lanServer.arg("tb_server");
    String tb_token = lanServer.arg("tb_token");
    String ip_mode_str = lanServer.arg("ip_mode");
    strncpy(netConfig.tb_server, tb_server.c_str(), sizeof(netConfig.tb_server) - 1);
    strncpy(netConfig.tb_token, tb_token.c_str(), sizeof(netConfig.tb_token) - 1);
    netConfig.ip_mode = (uint8_t)ip_mode_str.toInt();
    if (netConfig.ip_mode == 1) {
      String ip = lanServer.arg("ip");
      String subnet = lanServer.arg("subnet");
      String gw = lanServer.arg("gw");
      String dns = lanServer.arg("dns");
      strncpy(netConfig.static_ip, ip.c_str(), sizeof(netConfig.static_ip) - 1);
      strncpy(netConfig.subnet, subnet.c_str(), sizeof(netConfig.subnet) - 1);
      strncpy(netConfig.gateway, gw.c_str(), sizeof(netConfig.gateway) - 1);
      strncpy(netConfig.dns, dns.c_str(), sizeof(netConfig.dns) - 1);
    }
    saveNetworkConfig(netConfig);
    lanServer.sendHeader("Connection", "close");
    lanServer.send(200, "text/html", "<h1>Saved! Restarting...</h1>");
    delay(1000);
    ESP.restart();
  });
  lanServer.begin();
  lan_server_active = true;
  Serial.println("[LAN-WEB] Config server started on port 80");
  Serial.flush();
}

inline void handleLANServer() {
  if (lan_server_active) {
    lanServer.handleClient();
  }
}

#endif // LAN_WEBCONFIG_H