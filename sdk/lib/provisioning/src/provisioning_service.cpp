
#include "provisioning_service.h"

ProvisioningService::ProvisioningService(WebServer& server, DNSServer& dns, Preferences& prefs)
: _srv(server), _dns(dns), _prefs(prefs) {}

void ProvisioningService::load(McpConfig& cfg){
  _prefs.begin("mcp", true);
  if (_prefs.isKey("wifi_ssid"))  cfg.wifi_ssid = _prefs.getString("wifi_ssid");
  if (_prefs.isKey("wifi_pass"))  cfg.wifi_pass = _prefs.getString("wifi_pass");
  if (_prefs.isKey("mqtt_host"))  cfg.mqtt_host = _prefs.getString("mqtt_host");
  cfg.mqtt_port = _prefs.isKey("mqtt_port") ? _prefs.getUShort("mqtt_port") : 1883;
  if (_prefs.isKey("device_id"))  cfg.device_id = _prefs.getString("device_id");
  _prefs.end();
}

void ProvisioningService::save(const McpConfig& cfg){
  _prefs.begin("mcp", false);
  _prefs.putString("wifi_ssid", cfg.wifi_ssid);
  _prefs.putString("wifi_pass", cfg.wifi_pass);
  _prefs.putString("mqtt_host", cfg.mqtt_host);
  _prefs.putUShort("mqtt_port", cfg.mqtt_port);
  _prefs.putString("device_id", cfg.device_id);
  _prefs.end();
}

void ProvisioningService::clear(){
  _prefs.begin("mcp", false);
  _prefs.clear();
  _prefs.end();
}

bool ProvisioningService::hasMinimum(const McpConfig& cfg){
  return cfg.wifi_ssid.length() && cfg.mqtt_host.length();
}

String ProvisioningService::apSsid(){
  uint8_t mac[6]; WiFi.macAddress(mac);
  char ssid[32]; snprintf(ssid,sizeof(ssid),"MCP-SETUP-%02X%02X", mac[4], mac[5]);
  return String(ssid);
}

String ProvisioningService::htmlEscape(const String& s){
  String o; o.reserve(s.length()+8);
  for(char c: s){
    switch(c){
      case '&': o += F("&amp;"); break;
      case '<': o += F("&lt;"); break;
      case '>': o += F("&gt;"); break;
      case '\"': o += F("&quot;"); break;
      case '\'': o += F("&#39;"); break;
      default: o += c;
    }
  }
  return o;
}

String ProvisioningService::buildProvisionPage(const String& did, bool doScan){
  String body;
  body.reserve(6000);
  body += F("<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>MCP-Lite Setup</title><style>body{font-family:sans-serif;max-width:560px;margin:20px auto;padding:0 12px}label{display:block;margin:.6rem 0 .2rem}input,select{width:100%;padding:.6rem;font-size:1rem}button{padding:.6rem 1rem;margin-top:1rem}small{color:#666}</style></head><body><h2>MCP-Lite Provisioning</h2>");
  if(doScan){
    int n = WiFi.scanNetworks();
    body += F("<details open><summary>Scan Wi-Fi</summary><label>SSID</label><select id='ssid'>");
    for(int i=0;i<n;i++){
      String ssid = WiFi.SSID(i);
      int32_t rssi = WiFi.RSSI(i);
      bool enc = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
      body += "<option value='" + htmlEscape(ssid) + "'>";
      body += htmlEscape(ssid) + " (" + String(rssi) + " dBm";
      body += enc ? ", 🔒" : ", 🔓";
      body += ")</option>";
    }
    if(n<=0) body += F("<option value=''>No networks found (rescan)</option>");
    body += F("</select><button onclick=\"document.getElementById('ssidText').value=document.getElementById('ssid').value\">Use selected</button></details>");
  } else {
    body += F("<p><a href='/?scan=1'>Scan Wi-Fi</a> (recommended)</p>");
  }
  body += F("<form method='POST' action='/save'>"
            "<label>Wi-Fi SSID</label><input id='ssidText' name='wifi_ssid' required>"
            "<label>Wi-Fi Password</label><input name='wifi_pass' type='password'>"
            "<label>MQTT Host (IP)</label><input name='mqtt_host' value='192.168.0.100' required>"
            "<label>MQTT Port</label><input name='mqtt_port' type='number' value='1883' min='1' max='65535' required>"
            "<label>Device ID</label><input name='device_id' value='");
  body += htmlEscape(did);
  body += F("' required>"
            "<button type='submit'>Save & Reboot</button>"
            "</form><hr><p><small>Project mcp-lite</small></p></body></html>");
  return body;
}

void ProvisioningService::startPortal(const String& defaultDid){
  WiFi.mode(WIFI_AP);
  String ssid = apSsid();
  WiFi.softAP(ssid.c_str(), "12345678");
  
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  Serial.printf("[PROV] TX power set to %.1f dBm\n", WiFi.getTxPower() * 0.25f);


  delay(200);
  IPAddress apIP = WiFi.softAPIP();

  Serial.printf("[PROV] AP SSID=%s PASS=12345678 IP=%s\\n",
                ssid.c_str(), apIP.toString().c_str());

  _dns.start(53, "*", apIP);

  _srv.onNotFound([this, defaultDid](){
    bool doScan = _srv.hasArg("scan");
    String html = buildProvisionPage(defaultDid, doScan);
    _srv.send(200, "text/html; charset=utf-8", html);
  });
  _srv.on("/", HTTP_GET, [this, defaultDid](){
    String html = buildProvisionPage(defaultDid, _srv.hasArg("scan"));
    _srv.send(200, "text/html; charset=utf-8", html);
  });
  _srv.on("/generate_204", HTTP_GET, [this](){ _srv.send(204); });
  _srv.on("/hotspot-detect.html", HTTP_GET, [this](){ _srv.send(200, "text/plain", "OK"); });
  _srv.on("/save", HTTP_POST, [this](){
    McpConfig cfg;
    cfg.wifi_ssid = _srv.arg("wifi_ssid");
    cfg.wifi_pass = _srv.arg("wifi_pass");
    cfg.mqtt_host = _srv.arg("mqtt_host");
    cfg.mqtt_port = (uint16_t) _srv.arg("mqtt_port").toInt();
    cfg.device_id = _srv.arg("device_id");
    if(!cfg.wifi_ssid.length() || !cfg.mqtt_host.length() || !cfg.mqtt_port || !cfg.device_id.length()){
      _srv.send(422, "text/plain", "Missing required fields"); return;
    }
    save(cfg);
    _srv.send(200, "text/plain", "Saved. Rebooting...");
    delay(800); ESP.restart();
  });

  _srv.begin();
}

bool ProvisioningService::connectSTA(const String& ssid, const String& pass, unsigned long timeoutMs){
  WiFi.setSleep(false);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());
  unsigned long start = millis();
  while(WiFi.status()!=WL_CONNECTED && (millis()-start < timeoutMs)){ delay(500); }
  return WiFi.status()==WL_CONNECTED;
}
