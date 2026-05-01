// Captive Portal — WiFi Setup Only
// - Shows scanned WiFi networks as a selectable list
// - Password show/hide toggle
// - DHCP / Static IP selection
// - ThingsBoard settings managed separately (not here)
#ifndef CAPTIVE_PORTAL_H
#define CAPTIVE_PORTAL_H

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ArduinoJson.h>
#include "eth_config.h"

extern WebServer portalServer;
extern bool portal_active;
extern bool ap_mode_active;

static DNSServer dnsServer;

static const char PORTAL_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang='en'>
<head>
<meta charset='UTF-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>WiFi Setup</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,BlinkMacSystemFont,sans-serif;background:#111827;color:#e5e7eb;min-height:100vh}
.wrap{max-width:420px;margin:0 auto;padding:24px 16px 40px}
h1{color:#38bdf8;text-align:center;font-size:1.3em;margin-bottom:6px}
.sub{color:#6b7280;text-align:center;font-size:.85em;margin-bottom:20px}
.card{background:#1f2937;border-radius:14px;padding:20px;margin-bottom:14px;border:1px solid #374151}
.card-title{color:#38bdf8;font-size:.78em;font-weight:700;letter-spacing:.08em;text-transform:uppercase;margin-bottom:14px}
label{display:block;color:#9ca3af;font-size:.82em;margin-bottom:5px;margin-top:14px}
label:first-of-type{margin-top:0}
input[type=text],input[type=password]{
  width:100%;padding:11px 14px;border:1.5px solid #374151;border-radius:9px;
  background:#111827;color:#f3f4f6;font-size:15px;outline:none;transition:border .2s}
input:focus{border-color:#38bdf8}
.pw-wrap{position:relative}
.pw-wrap input{padding-right:46px}
.eye{position:absolute;right:12px;top:50%;transform:translateY(-50%);
     background:none;border:none;cursor:pointer;color:#6b7280;font-size:19px;
     padding:0;line-height:1;transition:color .2s}
.eye:hover{color:#38bdf8}
.hdr-row{display:flex;align-items:center;justify-content:space-between;margin-bottom:14px}
.scan-btn{background:none;border:1.5px solid #38bdf8;color:#38bdf8;padding:5px 12px;
          border-radius:7px;cursor:pointer;font-size:.82em;display:flex;align-items:center;gap:5px;transition:background .2s}
.scan-btn:hover{background:#38bdf81a}
.spin{display:inline-block;animation:spin .7s linear infinite}
@keyframes spin{to{transform:rotate(360deg)}}
.net-box{border:1.5px solid #374151;border-radius:9px;overflow:hidden;max-height:220px;overflow-y:auto;margin-bottom:4px}
.net-item{display:flex;align-items:center;justify-content:space-between;padding:11px 14px;
          cursor:pointer;border-bottom:1px solid #374151;transition:background .15s}
.net-item:last-child{border-bottom:none}
.net-item:hover,.net-item:active{background:#374151}
.net-name{font-size:.95em;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;max-width:220px}
.net-right{display:flex;align-items:center;gap:8px;flex-shrink:0}
.rssi{font-size:.75em;color:#6b7280}
.bars{display:flex;align-items:flex-end;gap:2px;height:14px}
.bars span{width:4px;border-radius:1px;background:#374151}
.bars span.on{background:#38bdf8}
.b1{height:30%}.b2{height:55%}.b3{height:78%}.b4{height:100%}
.lock{font-size:.78em;color:#f87171}
.msg{color:#6b7280;font-size:.85em;text-align:center;padding:18px}
.radio-row{display:flex;gap:20px;margin-top:2px}
.radio-row label{display:flex;align-items:center;gap:7px;color:#d1d5db;font-size:.9em;cursor:pointer;margin-top:0}
.radio-row input[type=radio]{accent-color:#38bdf8;width:16px;height:16px}
.static-box{display:none;margin-top:12px;border-top:1px solid #374151;padding-top:12px}
.save-btn{width:100%;padding:14px;background:#38bdf8;color:#0f172a;border:none;border-radius:10px;
          font-size:16px;font-weight:700;cursor:pointer;margin-top:6px;transition:background .2s}
.save-btn:hover{background:#0ea5e9}
.ok{text-align:center;padding:60px 20px}
.ok h2{color:#38bdf8;font-size:1.6em;margin-bottom:10px}
.ok p{color:#9ca3af;margin-top:8px}
</style>
</head>
<body>
<div class='wrap'>
  <h1>&#x1F4F6; WiFi Setup</h1>
  <p class='sub'>SLXA1260004 — Connect to your network</p>

  <form id='frm' action='/save' method='POST'>
  <div class='card'>
    <div class='hdr-row'>
      <span class='card-title'>WiFi Network</span>
      <button type='button' class='scan-btn' id='scanBtn' onclick='doScan()'>
        <span id='scanIco'>&#x21BB;</span> Scan
      </button>
    </div>

    <div id='netBox' class='net-box' style='display:none'></div>

    <label>SSID</label>
    <input type='text' id='ssidIn' name='ssid' placeholder='Select from list or type here' required autocomplete='off'>

    <label>Password</label>
    <div class='pw-wrap'>
      <input type='password' id='passIn' name='pass' placeholder='WiFi Password' autocomplete='off'>
      <button type='button' class='eye' id='eyeBtn' onclick='toggleEye()' title='Show/hide password'>
        &#x1F441;
      </button>
    </div>
  </div>

  <div class='card'>
    <div class='card-title'>IP Configuration</div>
    <div class='radio-row'>
      <label><input type='radio' name='ip_mode' value='0' checked onchange='toggleStatic(false)'> DHCP (Auto)</label>
      <label><input type='radio' name='ip_mode' value='1' onchange='toggleStatic(true)'> Static IP</label>
    </div>
    <div class='static-box' id='staticBox'>
      <label>IP Address</label>
      <input type='text' name='ip' placeholder='192.168.1.100' pattern='^(\d{1,3}\.){3}\d{1,3}$'>
      <label>Subnet Mask</label>
      <input type='text' name='subnet' placeholder='255.255.255.0'>
      <label>Gateway</label>
      <input type='text' name='gw' placeholder='192.168.1.1'>
      <label>DNS Server</label>
      <input type='text' name='dns' placeholder='8.8.8.8'>
    </div>
  </div>

  <button type='submit' class='save-btn'>&#x2713;&nbsp; Save &amp; Connect</button>
  </form>
</div>

<script>
var nets = [];
var eyeOpen = false;

function toggleEye() {
  eyeOpen = !eyeOpen;
  document.getElementById('passIn').type = eyeOpen ? 'text' : 'password';
  document.getElementById('eyeBtn').innerHTML = eyeOpen ? '&#x1F648;' : '&#x1F441;';
}

function toggleStatic(show) {
  document.getElementById('staticBox').style.display = show ? 'block' : 'none';
}

function rssiBar(r) {
  var l = r >= -50 ? 4 : r >= -65 ? 3 : r >= -75 ? 2 : 1;
  return '<div class="bars">' +
    '<span class="b1' + (l>=1?' on':'') + '"></span>' +
    '<span class="b2' + (l>=2?' on':'') + '"></span>' +
    '<span class="b3' + (l>=3?' on':'') + '"></span>' +
    '<span class="b4' + (l>=4?' on':'') + '"></span>' +
    '</div>';
}

function esc(s) {
  return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;');
}

function pickNet(i) {
  document.getElementById('ssidIn').value = nets[i].ssid;
  document.getElementById('netBox').style.display = 'none';
  document.getElementById('passIn').focus();
}

function doScan() {
  var box = document.getElementById('netBox');
  var ico = document.getElementById('scanIco');
  box.style.display = 'block';
  box.innerHTML = '<div class="msg"><span class="spin">&#x21BB;</span> Scanning...</div>';
  ico.className = 'spin';
  fetch('/scan')
    .then(function(r){ return r.json(); })
    .then(function(data) {
      ico.className = '';
      ico.innerHTML = '&#x21BB;';
      nets = data;
      if (!nets.length) { box.innerHTML = '<div class="msg">No networks found — try again</div>'; return; }
      box.innerHTML = nets.map(function(n,i) {
        return '<div class="net-item" onclick="pickNet(' + i + ')">' +
          '<span class="net-name">' + esc(n.ssid) + '</span>' +
          '<span class="net-right">' +
            rssiBar(n.rssi) +
            '<span class="rssi">' + n.rssi + ' dBm</span>' +
            (n.enc ? '<span class="lock">&#x1F512;</span>' : '') +
          '</span></div>';
      }).join('');
    })
    .catch(function() {
      ico.className = ''; ico.innerHTML = '&#x21BB;';
      box.innerHTML = '<div class="msg">Scan failed — tap Scan to retry</div>';
    });
}
</script>
</body>
</html>
)rawliteral";

// ── Saved confirmation page ───────────────────────────────────────────────
static const char PORTAL_SAVED_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html>
<head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>
<title>Saved</title>
<style>
body{font-family:sans-serif;background:#111827;color:#e5e7eb;display:flex;
     align-items:center;justify-content:center;min-height:100vh;margin:0}
.box{text-align:center;padding:40px 24px}
h2{color:#38bdf8;font-size:2em;margin-bottom:12px}
p{color:#9ca3af;margin:6px 0;font-size:.95em}
.ssid{color:#f3f4f6;font-weight:600}
</style>
</head>
<body>
<div class='box'>
  <div style='font-size:3em'>&#x2705;</div>
  <h2>Saved!</h2>
  <p>Connecting to <span class='ssid' id='s'></span></p>
  <p style='margin-top:16px'>Device will restart now...</p>
</div>
<script>
var p = new URLSearchParams(window.location.search);
document.getElementById('s').textContent = p.get('ssid') || '';
</script>
</body></html>
)rawliteral";

// ════════════════════════════════════════════════════════════════════════
inline void startCaptivePortal() {
  Serial.println("[PORTAL] Starting — WIFI_AP_STA (scan + AP)");

  // AP_STA mode so WiFi.scanNetworks() works while serving the portal
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP("Slix-Setup", "");
  ap_mode_active = true;
  portal_active  = true;

  dnsServer.start(53, "*", WiFi.softAPIP());

  // ── Main page ──
  portalServer.on("/", HTTP_GET, []() {
    portalServer.sendHeader("Connection", "close");
    portalServer.send(200, "text/html", PORTAL_HTML);
  });

  // ── WiFi scan — returns JSON array ──
  portalServer.on("/scan", HTTP_GET, []() {
    int n = WiFi.scanNetworks(false, false); // blocking scan, no hidden nets
    DynamicJsonDocument doc(4096);
    JsonArray arr = doc.to<JsonArray>();
    for (int i = 0; i < n; i++) {
      if (WiFi.SSID(i).length() == 0) continue; // skip hidden
      JsonObject o = arr.createNestedObject();
      o["ssid"] = WiFi.SSID(i);
      o["rssi"] = WiFi.RSSI(i);
      o["enc"]  = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
    }
    WiFi.scanDelete();
    String json; serializeJson(doc, json);
    portalServer.sendHeader("Connection", "close");
    portalServer.sendHeader("Access-Control-Allow-Origin", "*");
    portalServer.send(200, "application/json", json);
  });

  // ── Save config ──
  portalServer.on("/save", HTTP_POST, []() {
    String ssid       = portalServer.arg("ssid");
    String pass       = portalServer.arg("pass");
    String ip_mode_str = portalServer.arg("ip_mode");

    if (ssid.length() == 0) {
      portalServer.send(400, "text/plain", "SSID required");
      return;
    }

    strncpy(netConfig.wifi_ssid, ssid.c_str(), sizeof(netConfig.wifi_ssid) - 1);
    strncpy(netConfig.wifi_pass, pass.c_str(), sizeof(netConfig.wifi_pass) - 1);
    netConfig.wifi_ssid[sizeof(netConfig.wifi_ssid) - 1] = '\0';
    netConfig.wifi_pass[sizeof(netConfig.wifi_pass) - 1] = '\0';
    netConfig.ip_mode = (uint8_t)ip_mode_str.toInt();

    if (netConfig.ip_mode == 1) {
      String ip     = portalServer.arg("ip");
      String subnet = portalServer.arg("subnet");
      String gw     = portalServer.arg("gw");
      String dns    = portalServer.arg("dns");
      strncpy(netConfig.static_ip, ip.c_str(),     sizeof(netConfig.static_ip) - 1);
      strncpy(netConfig.subnet,    subnet.c_str(),  sizeof(netConfig.subnet) - 1);
      strncpy(netConfig.gateway,   gw.c_str(),      sizeof(netConfig.gateway) - 1);
      strncpy(netConfig.dns,       dns.c_str(),     sizeof(netConfig.dns) - 1);
    }

    saveNetworkConfig(netConfig);
    Serial.printf("[PORTAL] Saved SSID='%s' ip_mode=%d\n",
                  netConfig.wifi_ssid, netConfig.ip_mode);
    Serial.flush();

    portalServer.sendHeader("Connection", "close");
    portalServer.send(200, "text/html", PORTAL_SAVED_HTML);
    delay(2000);
    ESP.restart();
  });

  // ── Redirect everything else to portal ──
  portalServer.onNotFound([]() {
    portalServer.sendHeader("Location", "http://192.168.4.1/", true);
    portalServer.send(302, "text/plain", "");
  });

  portalServer.begin();
  Serial.printf("[PORTAL] AP: 'Slix-Setup' | IP: %s\n",
                WiFi.softAPIP().toString().c_str());
  Serial.println("[PORTAL] Open browser → http://192.168.4.1");
  Serial.flush();
}

inline void handlePortal() {
  if (portal_active) {
    dnsServer.processNextRequest();
    portalServer.handleClient();
  }
}

#endif // CAPTIVE_PORTAL_H
