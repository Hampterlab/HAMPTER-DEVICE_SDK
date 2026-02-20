#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include "esp_camera.h"
#include "mbedtls/md.h"

#include "config.hpp"
#include "transports/topics.h"
#include "registry.h"
#include "hooks.h"
#include "provisioning_service.h"
#include "tool.h"
#include "port_registry.h"   // 포트 시스템

// ==== FreeRTOS ====
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#ifndef STRICT_SIGNED_COMMANDS
#define STRICT_SIGNED_COMMANDS 0
#endif

// ========= Constants =========
static const uint16_t HTTP_PORT_NUM             = HTTP_PORT;
static const uint32_t MQTT_RECONNECT_INTERVAL   = 3000;
static const uint32_t WIFI_RECONNECT_INTERVAL   = 5000;
static const uint32_t STATUS_PUBLISH_INTERVAL   = 30000;
static const uint32_t ANNOUNCE_PUBLISH_INTERVAL = 300000;
static const uint32_t WIFI_DEBUG_INTERVAL       = 5000;
static const bool     REQUIRE_VALID_SIGNATURE    = (STRICT_SIGNED_COMMANDS != 0);

// ========= Run Mode =========
enum RunMode { MODE_PROVISION, MODE_RUN };
RunMode RUN_MODE = MODE_PROVISION;

// ========= Global Objects =========
WebServer        server(HTTP_PORT_NUM);
DNSServer        dnsServer;
Preferences      prefs;
WiFiClient       wifiClient;
PubSubClient     mqtt(wifiClient);
ToolRegistry     registry;
ProvisioningService* prov = nullptr;
McpConfig        CFG;
PortRegistry     g_portRegistry;

// ========= State Variables =========
String http_base;
String device_id;

// ========= Timing =========
unsigned long lastStatusMs   = 0;
unsigned long lastAnnounceMs = 0;
unsigned long lastMqttTry    = 0;
unsigned long lastWifiTry    = 0;
unsigned long lastWifiDbg    = 0;

// ========= RTOS: Tool Job Queue & MQTT Mutex =========
struct ToolJob {
  size_t len;           // JSON 길이
  char   payload[768];  // JSON 버퍼 (필요하면 크기 늘려도 됨)
};

static QueueHandle_t     g_toolJobQueue = nullptr;
static SemaphoreHandle_t g_mqttMutex    = nullptr;

// Forward declarations used by helper functions defined above MQTT helper section.
bool mqttPublishSafe(const String& topic, const String& msg, bool retain);

// ========= Helper Functions =========
String isoNow() {
  time_t now = time(nullptr);
  struct tm* t = gmtime(&now);
  char buf[32];
  if (t) strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", t);
  else   snprintf(buf, sizeof(buf), "1970-01-01T00:00:00Z");
  return String(buf);
}

String macTailDeviceId() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char buf[32];
  snprintf(buf, sizeof(buf), "dev-%02X%02X%02X", mac[3], mac[4], mac[5]);
  return String(buf);
}

// ========= MQTT Topics =========
inline String topicAnn()         { return topicAnnounce(device_id); }
inline String topicStat()        { return topicStatus(device_id); }
inline String topicCmdDev()      { return topicCmd(device_id); }
inline String topicEvt()         { return topicEvents(device_id); }
inline String topicPortsAnnDev() { return topicPortsAnnounce(device_id); }
inline String topicPortsDataDev(){ return topicPortsData(device_id); }
inline String topicPortsSetDev() { return topicPortsSet(device_id); }
inline String topicClaimDev()    { return topicClaim(device_id); }

String loadClaimToken() {
  prefs.begin("mcp", true);
  String tok = prefs.getString("claim_token", "");
  prefs.end();
  return tok;
}

void saveClaimToken(const String& token) {
  prefs.begin("mcp", false);
  prefs.putString("claim_token", token);
  prefs.end();
}

bool secureHexEquals(const String& a, const String& b) {
  if (a.length() != b.length()) return false;
  uint8_t diff = 0;
  for (size_t i = 0; i < a.length(); ++i) {
    char ca = a[i];
    char cb = b[i];
    if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
    if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
    diff |= (uint8_t)(ca ^ cb);
  }
  return diff == 0;
}

String hmacSha256Hex(const String& key, const String& data) {
  unsigned char out[32];
  const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!md) return String();

  int rc = mbedtls_md_hmac(
    md,
    (const unsigned char*)key.c_str(), key.length(),
    (const unsigned char*)data.c_str(), data.length(),
    out
  );
  if (rc != 0) return String();

  static const char* hex = "0123456789abcdef";
  char buf[65];
  for (int i = 0; i < 32; ++i) {
    buf[i * 2]     = hex[(out[i] >> 4) & 0x0F];
    buf[i * 2 + 1] = hex[out[i] & 0x0F];
  }
  buf[64] = '\0';
  return String(buf);
}

bool enqueueToolJobJson(const String& json) {
  if (!g_toolJobQueue) {
    Serial.println("[MQTT] Tool job queue not ready, dropping command");
    return false;
  }
  if (json.length() >= sizeof(ToolJob::payload)) {
    Serial.println("[MQTT] Normalized command too large for ToolJob, dropped");
    return false;
  }

  ToolJob job;
  job.len = json.length();
  memcpy(job.payload, json.c_str(), job.len);

  if (xQueueSend(g_toolJobQueue, &job, 0) != pdTRUE) {
    Serial.println("[MQTT] Tool job queue full, dropped");
    return false;
  }
  Serial.println("[MQTT] Tool job enqueued");
  return true;
}

bool normalizeCommandPayload(const byte* payload, unsigned length, const String& claimToken, String& outJson) {
  StaticJsonDocument<1024> outer;
  DeserializationError err = deserializeJson(outer, payload, length);
  if (err) {
    Serial.printf("[MQTT] command JSON parse error: %s\n", err.c_str());
    return false;
  }

  JsonVariantConst dataVar = outer["data"];
  JsonVariantConst sigVar = outer["signature"];

  // Accept multiple envelope encodings: const char*, String, or nested object.
  String data;
  String sig;
  if (dataVar.is<const char*>()) data = String(dataVar.as<const char*>());
  else if (dataVar.is<String>()) data = dataVar.as<String>();
  else if (dataVar.is<JsonObjectConst>()) serializeJson(dataVar.as<JsonObjectConst>(), data);

  if (sigVar.is<const char*>()) sig = String(sigVar.as<const char*>());
  else if (sigVar.is<String>()) sig = sigVar.as<String>();

  if (data.length() > 0 && sig.length() > 0) {
    Serial.printf("[SEC] envelope detected data_len=%u sig_len=%u\n", data.length(), sig.length());

    bool sigOk = false;
    if (claimToken.length() > 0) {
      String computed = hmacSha256Hex(claimToken, data);
      sigOk = computed.length() > 0 && secureHexEquals(computed, sig);
      if (sigOk) {
        Serial.println("[SEC] Signed command verified");
      } else {
        Serial.println("[SEC] Signature mismatch for signed command");
      }
    } else {
      Serial.println("[SEC] No claim token stored; accepting signed envelope in compatibility mode");
    }

    if (REQUIRE_VALID_SIGNATURE && claimToken.length() > 0 && !sigOk) {
      Serial.println("[SEC] Dropped command (STRICT_SIGNED_COMMANDS=1)");
      return false;
    }

    StaticJsonDocument<768> inner;
    DeserializationError iErr = deserializeJson(inner, data);
    if (iErr) {
      Serial.printf("[SEC] inner command parse error: %s\n", iErr.c_str());
      return false;
    }
    const char* typ = inner["type"] | "";
    if (strcmp(typ, "device.command") != 0) {
      Serial.printf("[MQTT] unsupported wrapped command type='%s'\n", typ);
      return false;
    }

    serializeJson(inner, outJson);
    return true;
  }

  // Compatibility 1: wrapped payload object { "payload": { ...device.command... } }
  JsonVariantConst payloadVar = outer["payload"];
  if (payloadVar.is<JsonObjectConst>()) {
    JsonObjectConst innerObj = payloadVar.as<JsonObjectConst>();
    const char* pTyp = innerObj["type"] | "";
    if (strcmp(pTyp, "device.command") == 0 ||
        (innerObj["tool"].is<const char*>() && innerObj["request_id"].is<const char*>())) {
      StaticJsonDocument<768> normalized;
      normalized.set(innerObj);
      if (!normalized["type"].is<const char*>()) {
        normalized["type"] = "device.command";
      }
      serializeJson(normalized, outJson);
      return true;
    }
  }

  // Compatibility 2: type omitted but core fields present
  const char* typ = outer["type"] | "";
  bool looksLikeCommand = outer["tool"].is<const char*>() && outer["request_id"].is<const char*>();
  if (strcmp(typ, "device.command") != 0 && !looksLikeCommand) {
    String raw;
    raw.reserve(length + 1);
    for (unsigned i = 0; i < length; ++i) raw += (char)payload[i];
    Serial.printf("[MQTT] unsupported command type='%s' raw=%s\n", typ, raw.c_str());
    return false;
  }

  if (strcmp(typ, "device.command") != 0 && looksLikeCommand) {
    outer["type"] = "device.command";
  }
  serializeJson(outer, outJson);
  return true;
}

bool handleClaimTokenMessage(const byte* payload, unsigned length, String& claimToken) {
  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err) {
    Serial.printf("[SEC] claim JSON parse error: %s\n", err.c_str());
    return false;
  }
  const char* token = doc["token"] | nullptr;
  if (!token || !*token) {
    Serial.println("[SEC] claim message missing token");
    return false;
  }

  claimToken = String(token);
  saveClaimToken(claimToken);
  Serial.printf("[SEC] claim token updated (len=%u)\n", claimToken.length());
  return true;
}

String buildCommandErrorEvent(const String& requestId, const char* code, const char* message) {
  ObservationBuilder ob;
  ob.setRequestId(requestId.length() ? requestId : String(millis(), HEX));
  ob.error(code, message);
  return ob.toJson();
}

String extractRequestIdBestEffort(const byte* payload, unsigned length) {
  StaticJsonDocument<1024> outer;
  if (deserializeJson(outer, payload, length) != DeserializationError::Ok) {
    return String();
  }

  const char* rid = outer["request_id"] | nullptr;
  if (rid && *rid) return String(rid);

  const char* wrappedData = outer["data"] | nullptr;
  if (!wrappedData) return String();

  StaticJsonDocument<768> inner;
  if (deserializeJson(inner, wrappedData) != DeserializationError::Ok) {
    return String();
  }
  const char* innerRid = inner["request_id"] | nullptr;
  if (innerRid && *innerRid) return String(innerRid);
  return String();
}

void publishCommandError(const String& requestId, const char* code, const char* message) {
  String evt = buildCommandErrorEvent(requestId, code, message);
  bool ok = mqttPublishSafe(topicEvt(), evt, false);
  Serial.printf("[MQTT] Immediate error event %s (rid=%s code=%s)\n",
                ok ? "✓" : "✗",
                requestId.length() ? requestId.c_str() : "-",
                code);
}

// ========= WiFi TX Power Helper =========
// TX 파워를 항상 8.5 dBm으로 유지하면서 로그도 남김
void apply_wifi_tx_power() {
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  float tx = WiFi.getTxPower() * 0.25f;
  Serial.printf("[WIFI] TX power set to %.1f dBm\n", tx);
}

// ========= MQTT Helpers (RTOS-safe) =========
bool mqttPublishSafe(const String& topic, const String& msg, bool retain) {
  if (!mqtt.connected()) return false;
  if (g_mqttMutex) xSemaphoreTake(g_mqttMutex, portMAX_DELAY);
  bool ok = mqtt.publish(topic.c_str(), msg.c_str(), retain);
  if (g_mqttMutex) xSemaphoreGive(g_mqttMutex);
  return ok;
}

void mqttLoopSafe() {
  if (!mqtt.connected()) return;
  if (g_mqttMutex) xSemaphoreTake(g_mqttMutex, portMAX_DELAY);
  mqtt.loop();
  if (g_mqttMutex) xSemaphoreGive(g_mqttMutex);
}

// ========= OutPort에서 사용하는 helper =========
bool port_publish_data(const char* portName, float value) {
  if (!mqtt.connected()) {
    Serial.printf("[PORT] MQTT not connected, drop data port=%s\n", portName);
    return false;
  }

  StaticJsonDocument<128> doc;
  doc["port"]      = portName;
  doc["value"]     = value;
  doc["timestamp"] = isoNow();

  String payload;
  serializeJson(doc, payload);

  bool ok = mqttPublishSafe(topicPortsDataDev(), payload, false);
  
  return ok;
}

// ========= MQTT Publishing =========
void publishAnnounce() {
  if (!mqtt.connected()) return;

  String ann = registry.buildAnnounce(device_id, http_base);
  bool ok = mqttPublishSafe(topicAnn(), ann, true);
  Serial.printf("[MQTT] Announce %s (retain, %u bytes)\n",
                ok ? "✓" : "✗", ann.length());

  if (ok) lastAnnounceMs = millis();
}

void publishPortsAnnounce() {
  if (!mqtt.connected()) return;

  String ann = g_portRegistry.buildAnnounce(device_id);
  bool ok = mqttPublishSafe(topicPortsAnnDev(), ann, true);
  Serial.printf("[MQTT] Ports Announce %s (retain, %u bytes)\n",
                ok ? "✓" : "✗", ann.length());
}

void publishStatus(bool online) {
  if (!mqtt.connected()) return;

  StaticJsonDocument<256> doc;
  doc["type"]      = "device.status";
  doc["device_id"] = device_id;
  doc["online"]    = online;
  doc["uptime_ms"] = millis();
  doc["rssi"]      = WiFi.RSSI();
  doc["ts"]        = isoNow();

  String s;
  serializeJson(doc, s);
  bool ok = mqttPublishSafe(topicStat(), s, false);

  Serial.printf("[MQTT] Status %s (online=%d, rssi=%d, len=%u)\n",
                ok ? "✓" : "✗",
                online ? 1 : 0,
                (int)WiFi.RSSI(),
                s.length());

  if (ok) lastStatusMs = millis();
}

void clearRetainedMessages() {
  if (!mqtt.connected()) return;
  mqttPublishSafe(topicAnn(),        "", true);
  mqttPublishSafe(topicStat(),       "", true);
  mqttPublishSafe(topicPortsAnnDev(),"", true);
  Serial.println("[MQTT] Cleared retained announce/status/ports");
}

// ========= MQTT Connection =========
bool mqttConnect() {
  if (mqtt.connected()) return true;

  Serial.printf("[MQTT] Connecting to %s:%u...\n",
                CFG.mqtt_host.c_str(), CFG.mqtt_port);

  // Last Will
  StaticJsonDocument<256> will;
  will["type"]      = "device.status";
  will["device_id"] = device_id;
  will["online"]    = false;
  will["uptime_ms"] = millis();
  will["ts"]        = isoNow();

  char  willBuf[256];
  size_t willLen = serializeJson(will, willBuf, sizeof(willBuf));
  if (willLen >= sizeof(willBuf)) willBuf[sizeof(willBuf)-1] = '\0';
  else                            willBuf[willLen]           = '\0';

  mqtt.setServer(CFG.mqtt_host.c_str(), CFG.mqtt_port);

  if (g_mqttMutex) xSemaphoreTake(g_mqttMutex, portMAX_DELAY);
  bool ok = mqtt.connect(
    device_id.c_str(),         // client_id
    nullptr, nullptr,          // username/password
    topicStat().c_str(),       // will topic
    0,                         // will qos
    true,                      // will retain
    willBuf                    // will msg
  );
  if (!ok) {
    int st = mqtt.state();
    if (g_mqttMutex) xSemaphoreGive(g_mqttMutex);
    Serial.printf("[MQTT] Connect failed (state=%d)\n", st);
    return false;
  }

  String cmdTopic      = topicCmdDev();
  String portsSetTopic = topicPortsSetDev();
  String claimTopic    = topicClaimDev();

  bool subCmd  = mqtt.subscribe(cmdTopic.c_str());
  bool subPort = mqtt.subscribe(portsSetTopic.c_str());
  bool subClaim = mqtt.subscribe(claimTopic.c_str());

  if (g_mqttMutex) xSemaphoreGive(g_mqttMutex);

  Serial.printf("[MQTT] Connected & subscribed:\n");
  Serial.printf("       cmd       = '%s' (%s)\n",
                cmdTopic.c_str(),      subCmd  ? "OK" : "FAIL");
  Serial.printf("       ports/set = '%s' (%s)\n",
                portsSetTopic.c_str(), subPort ? "OK" : "FAIL");
  Serial.printf("       claim     = '%s' (%s)\n",
                claimTopic.c_str(),    subClaim ? "OK" : "FAIL");

  publishAnnounce();
  publishStatus(true);
  publishPortsAnnounce();

  return true;
}

// ========= HTTP Handlers =========
void setupHttpHandlers() {
  server.on("/", HTTP_GET, [](){
    String msg =
      "MCP-Lite Device API\n\n"
      "Endpoints:\n"
      "  GET /              - This help\n"
      "  GET /status_now    - Publish status immediately\n"
      "  GET /reannounce    - Re-publish announce + ports (retain)\n"
      "  GET /clear_retained - Clear retained messages\n"
      "  GET /factory_reset  - Factory reset & reboot\n";

    server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
    server.sendHeader("Pragma", "no-cache");
    server.send(200, "text/plain", msg);
  });

  server.on("/status_now", HTTP_GET, [](){
    if (!mqtt.connected()) {
      server.send(503, "text/plain", "MQTT not connected");
      return;
    }
    publishStatus(true);
    server.send(200, "text/plain", "Status published");
  });

  server.on("/reannounce", HTTP_GET, [](){
    if (!mqtt.connected()) {
      server.send(503, "text/plain", "MQTT not connected");
      return;
    }
    publishAnnounce();
    publishPortsAnnounce();
    server.send(200, "text/plain", "Announce + ports re-published (retain)");
  });

  server.on("/clear_retained", HTTP_GET, [](){
    if (!mqtt.connected()) {
      server.send(503, "text/plain", "MQTT not connected");
      return;
    }
    clearRetainedMessages();
    server.send(200, "text/plain", "Retained messages cleared");
  });

  server.on("/factory_reset", HTTP_GET, [](){
    prov->clear();
    if (mqtt.connected()) {
      clearRetainedMessages();
      if (g_mqttMutex) xSemaphoreTake(g_mqttMutex, portMAX_DELAY);
      mqtt.disconnect();
      if (g_mqttMutex) xSemaphoreGive(g_mqttMutex);
    }
    server.send(200, "text/plain", "Factory reset done. Rebooting...");
    delay(800);
    ESP.restart();
  });

  // 각 툴이 필요한 HTTP 엔드포인트 등록
  for (auto* t : registry.list()) {
    t->register_http(server);
    Serial.printf("[HTTP] Tool '%s' registered HTTP endpoints\n", t->name());
  }
}

// ========= Provisioning Mode =========
void startProvisioning() {
  RUN_MODE = MODE_PROVISION;
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  String did = CFG.device_id.length() ? CFG.device_id : macTailDeviceId();

  Serial.println("[PROV] Starting provisioning portal...");
  prov->startPortal(did);
  Serial.println("[PROV] Portal ready. Connect to the device AP.");
}

// ========= Runtime Mode =========
void startRuntime() {
  RUN_MODE = MODE_RUN;

  Serial.println("[RUN] Starting runtime mode...");

  // NTP
  configTime(9*3600, 0, "pool.ntp.org", "time.google.com");

  // HTTP 서버
  setupHttpHandlers();
  server.begin();
  Serial.printf("[HTTP] Server started on port %u\n", HTTP_PORT_NUM);

  // MQTT 설정
  mqtt.setBufferSize(2048);
  mqtt.setKeepAlive(60);
  static String claimToken = loadClaimToken();
  if (claimToken.length() > 0) {
    Serial.printf("[SEC] claim token loaded (len=%u)\n", claimToken.length());
  }

  mqtt.setCallback([](char* topic, byte* payload, unsigned length){
    Serial.printf("[MQTT] RX topic='%s' (%u bytes)\n", topic, length);
    String t(topic);

    // 1) ports/set 처리 → InPort 값 변경
    if (t == topicPortsSetDev()) {
      StaticJsonDocument<256> doc;
      DeserializationError err = deserializeJson(doc, payload, length);
      if (err) {
        Serial.printf("[MQTT] ports.set JSON parse error: %s\n", err.c_str());
        return;
      }
      const char* portName = doc["port"] | nullptr;
      float value          = doc["value"] | 0.0f;
      if (!portName) {
        Serial.println("[MQTT] ports.set missing 'port'");
        return;
      }

      g_portRegistry.handleInPortSet(String(portName), value);
      return;
    }

    // 2) claim 토큰 처리
    if (t == topicClaimDev()) {
      handleClaimTokenMessage(payload, length, claimToken);
      return;
    }

    // 2) device.command → ToolWorker 큐로 전달
    if (t == topicCmdDev()) {
      String rid = extractRequestIdBestEffort(payload, length);
      String normalized;
      if (!normalizeCommandPayload(payload, length, claimToken, normalized)) {
        publishCommandError(rid, "invalid_command", "invalid or unauthorized command payload");
        return;
      }
      if (!enqueueToolJobJson(normalized)) {
        publishCommandError(rid, "busy", "command queue is full");
      }
      return;
    }

    Serial.println("[MQTT] Unknown topic, ignored");
  });

  if (!mqttConnect()) {
    Serial.println("[RUN] MQTT initial connect failed, will retry...");
  }

  Serial.println("[RUN] Runtime mode ready");
}

// ========= RTOS: Tool Worker Task =========
void ToolWorkerTask(void* pv) {
  (void)pv;
  Serial.println("[TOOL] Worker task started");

  for (;;) {
    ToolJob job;
    if (xQueueReceive(g_toolJobQueue, &job, portMAX_DELAY) != pdTRUE) {
      continue;
    }

    StaticJsonDocument<768> cmd;
    DeserializationError err = deserializeJson(cmd, job.payload, job.len);
    if (err) {
      Serial.printf("[TOOL] JSON parse error in worker: %s\n", err.c_str());
      continue;
    }

    const char* type     = cmd["type"] | "unknown";
    const char* toolName = cmd["tool"] | "unknown";
    Serial.printf("[TOOL] Handling cmd type=%s, tool=%s\n", type, toolName);

    String eventsJson;
    bool dispatched = registry.dispatch(cmd, eventsJson, http_base);

    if (!dispatched) {
      Serial.println("[TOOL] Dispatch failed (tool not found or error)");
      Serial.printf("[TOOL]   -> eventsJson: %s\n", eventsJson.c_str());
    }

    // assets URL 패치 (상대경로 → http_base 붙이기)
    StaticJsonDocument<2048> tmp;
    DeserializationError err2 = deserializeJson(tmp, eventsJson);
    if (err2 == DeserializationError::Ok) {
      JsonArray assets = tmp["result"]["assets"];
      if (!assets.isNull()) {
        for (JsonObject a : assets) {
          const char* url = a["url"] | nullptr;
          if (url && url[0] == '/') {
            a["url"] = http_base + String(url);
          }
        }
        String patched;
        serializeJson(tmp, patched);
        eventsJson = patched;
      }
    }

    bool ok = mqttPublishSafe(topicEvt(), eventsJson, false);
    Serial.printf("[MQTT] Events %s (%u bytes)\n",
                  ok ? "✓" : "✗", eventsJson.length());
  }
}

// ========= Setup =========
void setup() {
  Serial.begin(115200);
  delay(300);

  Serial.println();
  Serial.println("╔══════════════════════════════════════╗");
  Serial.println("║    MCP-Lite Device Firmware (RTOS)   ║");
  Serial.println("║  Tools + Ports + MQTT + HTTP Debug   ║");
  Serial.println("╚══════════════════════════════════════╝");

  // RTOS 리소스
  g_mqttMutex    = xSemaphoreCreateMutex();
  g_toolJobQueue = xQueueCreate(4, sizeof(ToolJob));
  if (!g_toolJobQueue) {
    Serial.println("[RTOS] FAILED to create ToolJob queue!");
  } else {
    Serial.println("[RTOS] ToolJob queue created");
  }

  // Provisioning 서비스
  prov = new ProvisioningService(server, dnsServer, prefs);
  prov->load(CFG);

  // Device ID
  device_id = CFG.device_id.length() ? CFG.device_id : macTailDeviceId();
  Serial.printf("[BOOT] Device ID: %s\n", device_id.c_str());

  // Tools 등록
  ToolConfig tc;
  register_tools(registry, tc);

  bool initOk = registry.initAll();
  Serial.printf("[BOOT] Tool registry: %u tools, init=%s\n",
                (unsigned)registry.list().size(),
                initOk ? "OK" : "FAILED");
  Serial.println("[BOOT] Registered tools:");
  for (auto* t : registry.list()) {
    Serial.printf("  - %s\n", t->name());
  }

  // Ports 등록 (modules 쪽 register_ports에서 실제 포트 추가)
  PortConfig pc;
  register_ports(g_portRegistry, pc);
  Serial.printf("[BOOT] Port registry: %u outports, %u inports\n",
                (unsigned)g_portRegistry.outportCount(),
                (unsigned)g_portRegistry.inportCount());
  
  // 설정이 없으면 Provisioning 모드
  if (!prov->hasMinimum(CFG)) {
    Serial.println("[BOOT] No config found, starting provisioning...");
    startProvisioning();
    return;
  }
  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  // Wi-Fi STA 연결
  Serial.printf("[BOOT] Connecting to Wi-Fi '%s'...\n", CFG.wifi_ssid.c_str());
  if (!prov->connectSTA(CFG.wifi_ssid, CFG.wifi_pass)) {
    Serial.println("[BOOT] Wi-Fi connect failed, starting provisioning...");
    startProvisioning();
    return;
  }

  // ★ 최초 연결 후 TX 파워 적용
  apply_wifi_tx_power();

  IPAddress ip = WiFi.localIP();
  http_base = String("http://") + ip.toString();
  Serial.printf("[WIFI] Connected! IP=%s, RSSI=%d dBm\n",
                ip.toString().c_str(),
                (int)WiFi.RSSI());

  // Runtime 시작
  startRuntime();

  // ToolWorker Task 시작
  BaseType_t ok = xTaskCreate(
    ToolWorkerTask,
    "ToolWorker",
    4096,
    nullptr,
    1,
    nullptr
  );
  if (ok != pdPASS) {
    Serial.println("[RTOS] FAILED to create ToolWorker task!");
  } else {
    Serial.println("[RTOS] ToolWorker task created");
  }
}

// ========= Loop =========
void loop() {
  static wl_status_t lastWifiStatus = WL_IDLE_STATUS;
  uint32_t now = millis();

  // Provisioning mode -> captive portal
  if (RUN_MODE == MODE_PROVISION) {
    dnsServer.processNextRequest();
    server.handleClient();
    vTaskDelay(1);
    return;
  }

  // Runtime mode
  server.handleClient();

  wl_status_t curStatus = WiFi.status();

  // Wi-Fi reconnect
  if (curStatus != WL_CONNECTED) {
    if (now - lastWifiTry >= WIFI_RECONNECT_INTERVAL) {
      lastWifiTry = now;
      Serial.printf("[WIFI] Disconnected(status=%d), reconnecting...\n", (int)curStatus);
      WiFi.disconnect();
      delay(10); 
      WiFi.mode(WIFI_STA);
      WiFi.setTxPower(WIFI_POWER_8_5dBm); // 재연결 시도 전 파워 낮춤 필수
      WiFi.begin(CFG.wifi_ssid.c_str(), CFG.wifi_pass.c_str());
      
      Serial.println("[WIFI] Re-initiated connection with low TX power");
    }
  }

  // Wi-Fi 상태가 "끊겼다가 다시 붙은" 경우 TX 파워 다시 적용
  if (curStatus == WL_CONNECTED && lastWifiStatus != WL_CONNECTED) {
    Serial.println("[WIFI] Connected event detected, re-applying TX power");
    apply_wifi_tx_power();
  }
  lastWifiStatus = curStatus;

  // Wi-Fi 디버그 로그
  if (now - lastWifiDbg >= WIFI_DEBUG_INTERVAL) {
    lastWifiDbg = now;
    Serial.printf("[WIFI] status=%d, RSSI=%d dBm, TX=%.1f dBm\n",
                  (int)WiFi.status(),
                  (int)WiFi.RSSI(),
                  WiFi.getTxPower() * 0.25f);
  }

  // MQTT 관리
  if (!mqtt.connected()) {
    if (now - lastMqttTry >= MQTT_RECONNECT_INTERVAL &&
        WiFi.status() == WL_CONNECTED) {
      lastMqttTry = now;
      mqttConnect();
    }
  } else {
    mqttLoopSafe();

    if (now - lastStatusMs   >= STATUS_PUBLISH_INTERVAL)   publishStatus(true);
    if (now - lastAnnounceMs >= ANNOUNCE_PUBLISH_INTERVAL) {
      publishAnnounce();
      publishPortsAnnounce();
    }
  }

  // OutPort tick (센서 등)
  g_portRegistry.tickAll(now);

  vTaskDelay(1);
}
