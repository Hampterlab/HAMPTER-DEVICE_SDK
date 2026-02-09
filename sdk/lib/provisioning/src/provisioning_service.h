#pragma once
#include <Arduino.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <WiFi.h>

struct McpConfig {
  String wifi_ssid;
  String wifi_pass;
  String mqtt_host;
  uint16_t mqtt_port = 1883;
  String device_id;
};

class ProvisioningService {
public:
  ProvisioningService(WebServer& server, DNSServer& dns, Preferences& prefs);
  void load(McpConfig& cfg);
  void save(const McpConfig& cfg);
  void clear();
  bool hasMinimum(const McpConfig& cfg);
  void startPortal(const String& defaultDid);
  bool connectSTA(const String& ssid, const String& pass, unsigned long timeoutMs=30000);
  String apSsid();
private:
  WebServer& _srv;
  DNSServer& _dns;
  Preferences& _prefs;
  String htmlEscape(const String& s);
  String buildProvisionPage(const String& did, bool doScan);
};