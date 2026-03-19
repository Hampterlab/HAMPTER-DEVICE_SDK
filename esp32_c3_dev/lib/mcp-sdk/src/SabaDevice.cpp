#include "SabaDevice.h"
#include "topics.h"
#include "tool.h"
#include <mbedtls/md.h>

// ========= Global Instances & Helpers =========
PortRegistry g_portRegistry;
static SabaDevice* g_sabaDevice = nullptr;

String isoNow() {
  time_t now = time(nullptr);
  struct tm* t = gmtime(&now);
  char buf[32];
  if (t) strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", t);
  else   snprintf(buf, sizeof(buf), "1970-01-01T00:00:00Z");
  return String(buf);
}

bool port_publish_data(const char* portName, float value) {
    if (g_sabaDevice) {
        return g_sabaDevice->publishPortData(portName, value);
    }
    return false;
}

bool port_publish_state(const char* portName, float value, bool accepted, const char* source) {
    if (g_sabaDevice) {
        return g_sabaDevice->publishPortState(portName, value, accepted, source);
    }
    return false;
}

void port_set_outport_value(const char* portName, float value) {
    if (!portName || !*portName) return;
    port_publish_data(portName, value);
}

// ========= Constants =========
static const uint32_t MQTT_RECONNECT_INTERVAL   = 3000;
static const uint32_t WIFI_RECONNECT_INTERVAL   = 5000;
static const uint32_t STATUS_PUBLISH_INTERVAL   = 30000;
static const uint32_t ANNOUNCE_PUBLISH_INTERVAL = 300000;
static const uint32_t WIFI_DEBUG_INTERVAL       = 5000;

// Internal Job Struct for RTOS Queue
struct ToolJob {
  size_t len;
  char   payload[768];
};

SabaDevice::SabaDevice(const char* deviceName, const char* fwVersion)
    : _deviceName(deviceName), _fwVersion(fwVersion),
      _server(80),
      _mqtt(_wifiClient),
      _mainPortRegistry(g_portRegistry) // Bind to global
{
    g_sabaDevice = this;
}

SabaDevice::~SabaDevice() {
    if (_provisioning) delete _provisioning;
}

void SabaDevice::begin() {
    Serial.begin(115200);
    delay(300);

    Serial.println();
    Serial.println("╔══════════════════════════════════════╗");
    Serial.printf("║  %s (%s)\n", _deviceName.c_str(), _fwVersion.c_str());
    Serial.println("║  SABA Device SDK Initialized         ║");
    Serial.println("╚══════════════════════════════════════╝");

    // RTOS Resources
    _mqttMutex = xSemaphoreCreateRecursiveMutex();
    _jobQueue = xQueueCreate(4, sizeof(ToolJob));
    if (!_jobQueue) {
        Serial.println("[RTOS] FAILED to create ToolJob queue!");
    } else {
        Serial.println("[RTOS] ToolJob queue created");
    }

    // Provisioning
    _provisioning = new ProvisioningService(_server, _dnsServer, _prefs);
    _provisioning->load(_cfg);

    // Device ID resolution
    if (_cfg.device_id.length() > 0) {
        // Use configured ID
    } else {
        _cfg.device_id = macTailDeviceId();
    }
    Serial.printf("[BOOT] Device ID: %s\n", _cfg.device_id.c_str());

    // Tools Init
    bool initOk = _toolRegistry.initAll();
    Serial.printf("[BOOT] Tool registry: %u tools, init=%s\n", 
        (unsigned)_toolRegistry.list().size(), initOk ? "OK" : "FAILED");
    for (auto* t : _toolRegistry.list()) {
        Serial.printf("  - %s\n", t->name());
    }

    // Port Init
    Serial.printf("[BOOT] Port registry: %u outports, %u inports\n",
        (unsigned)_mainPortRegistry.outportCount(), (unsigned)_mainPortRegistry.inportCount());

    // Check Config
    if (!_provisioning->hasMinimum(_cfg)) {
        Serial.println("[BOOT] No config found, starting provisioning...");
        startProvisioning();
        return;
    }

    WiFi.mode(WIFI_STA);
    WiFi.setTxPower(WIFI_POWER_8_5dBm);
    Serial.printf("[BOOT] Connecting to Wi-Fi '%s'...\n", _cfg.wifi_ssid.c_str());
    
    if (!_provisioning->connectSTA(_cfg.wifi_ssid, _cfg.wifi_pass)) {
        Serial.println("[BOOT] Wi-Fi connect failed, starting provisioning...");
        startProvisioning();
        return;
    }

    applyWifiTxPower();

    IPAddress ip = WiFi.localIP();
    _httpBase = String("http://") + ip.toString();
    Serial.printf("[WIFI] Connected! IP=%s, RSSI=%d dBm\n", ip.toString().c_str(), (int)WiFi.RSSI());

    startRuntime();

    // Start Worker Task
    BaseType_t ok = xTaskCreate(
        toolWorkerTaskWrapper,
        "ToolWorker",
        4096,
        this, // Pass 'this' as context
        1,
        nullptr
    );
    if (ok != pdPASS) {
         Serial.println("[RTOS] FAILED to create ToolWorker task!");
    } else {
         Serial.println("[RTOS] ToolWorker task created");
    }
}

void SabaDevice::loop() {
    static int lastWifiStatus = WL_IDLE_STATUS;
    uint32_t now = millis();

    if (_isProvisionMode) {
        _dnsServer.processNextRequest();
        _server.handleClient();
        vTaskDelay(1);
        return;
    }

    // Runtime
    _server.handleClient();

    int curStatus = WiFi.status();
    if (curStatus != WL_CONNECTED) {
        if (now - _lastWifiTry >= WIFI_RECONNECT_INTERVAL) {
            _lastWifiTry = now;
            Serial.printf("[WIFI] Disconnected(status=%d), reconnecting...\n", curStatus);
            WiFi.disconnect();
            delay(10);
            WiFi.mode(WIFI_STA);
            WiFi.setTxPower(WIFI_POWER_8_5dBm);
            WiFi.begin(_cfg.wifi_ssid.c_str(), _cfg.wifi_pass.c_str());
            Serial.println("[WIFI] Re-initiated connection with low TX power");
        }
    }

    if (curStatus == WL_CONNECTED && lastWifiStatus != WL_CONNECTED) {
        Serial.println("[WIFI] Connected event detected, re-applying TX power");
        applyWifiTxPower();
    }
    lastWifiStatus = curStatus;

    if (now - _lastWifiDbg >= WIFI_DEBUG_INTERVAL) {
        _lastWifiDbg = now;
        Serial.printf("[WIFI] status=%d, RSSI=%d dBm, TX=%.1f dBm\n",
            (int)WiFi.status(), (int)WiFi.RSSI(), WiFi.getTxPower() * 0.25f);
    }

    if (!_mqtt.connected()) {
        if (now - _lastMqttTry >= MQTT_RECONNECT_INTERVAL && WiFi.status() == WL_CONNECTED) {
            _lastMqttTry = now;
            mqttConnect();
        }
    } else {
        mqttLoopSafe();
        if (now - _lastStatusMs >= STATUS_PUBLISH_INTERVAL) publishStatus(true);
        if (now - _lastAnnounceMs >= ANNOUNCE_PUBLISH_INTERVAL) {
            publishAnnounce();
            publishPortsAnnounce();
        }
    }

    _mainPortRegistry.tickAll(now);
    vTaskDelay(1);
}

void SabaDevice::setupHttpHandlers() {
    _server.on("/", HTTP_GET, [this](){
        String msg = "SABA Device API\n\nEndpoints:\n  GET /status_now\n  GET /reannounce\n  GET /factory_reset\n";
        _server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
        _server.send(200, "text/plain", msg);
    });

    _server.on("/status_now", HTTP_GET, [this](){
        if (!_mqtt.connected()) { _server.send(503, "text/plain", "MQTT not connected"); return; }
        publishStatus(true);
        _server.send(200, "text/plain", "Status published");
    });

    _server.on("/reannounce", HTTP_GET, [this](){
        if (!_mqtt.connected()) { _server.send(503, "text/plain", "MQTT not connected"); return; }
        publishAnnounce();
        publishPortsAnnounce();
        _server.send(200, "text/plain", "Re-announced");
    });

    _server.on("/factory_reset", HTTP_GET, [this](){
        _provisioning->clear();
        if (_mqtt.connected()) {
            clearRetainedMessages();
            mqttPublishSafe(topicStatus(_cfg.device_id), "", true); // Clear status
        }
        _server.send(200, "text/plain", "Factory reset done. Rebooting...");
        delay(800);
        ESP.restart();
    });

    for (auto* t : _toolRegistry.list()) {
        t->register_http(_server);
    }
}

void SabaDevice::startProvisioning() {
    _isProvisionMode = true;
    WiFi.setTxPower(WIFI_POWER_8_5dBm);
    String did = _cfg.device_id.length() ? _cfg.device_id : macTailDeviceId();
    Serial.println("[PROV] Starting provisioning portal...");
    _provisioning->startPortal(did);
    Serial.println("[PROV] Portal ready.");
}

void SabaDevice::startRuntime() {
    _isProvisionMode = false;
    Serial.println("[RUN] Starting runtime mode...");
    configTime(9*3600, 0, "pool.ntp.org", "time.google.com");

    setupHttpHandlers();
    _server.begin();
    
    _mqtt.setBufferSize(2048);
    _mqtt.setKeepAlive(60);
    _mqtt.setServer(_cfg.mqtt_host.c_str(), _cfg.mqtt_port);

    // Lambda for MQTT Params
    _mqtt.setCallback([this](char* topic, byte* payload, unsigned length) {
        String t(topic);
        // 0) Claim
        if (t == "mcp/dev/" + _cfg.device_id + "/claim") {
             if (_cfg.secret_token.length() > 0) return; // Already claimed
             StaticJsonDocument<256> doc;
             deserializeJson(doc, payload, length);
             const char* token = doc["token"];
             if (token && strlen(token) > 0) {
                 _cfg.secret_token = String(token);
                 _provisioning->save(_cfg);
                 Serial.println("[CLAIM] Device claimed!");
             }
             return;
        }
        // 1) Ports Set
        if (t == topicPortsSet(_cfg.device_id)) {
             String portName;
             float value = 0.0f;
             String rawPayload;
             if (!_mainPortRegistry.parseInPortSetPayload(payload, length, portName, value, &rawPayload)) {
                 return;
             }
             _mainPortRegistry.handleInPortSet(portName, value, "mqtt.ports.set", true);
             return;
        }
        // 2) Command
        if (t == topicCmd(_cfg.device_id)) {
            if (!_jobQueue) return;
            if (length >= 768) return; // Struct limit

            ToolJob job;
            StaticJsonDocument<1024> doc;
            DeserializationError err = deserializeJson(doc, payload, length);
            const char* dataStr = doc["data"];
            const char* sig = doc["signature"];
            bool isWrapped = (err == DeserializationError::Ok && dataStr && sig);

            if (isWrapped) {
                if (_cfg.secret_token.length() > 0) {
                     String calc = hmacSha256(_cfg.secret_token, String(dataStr));
                     if (calc != String(sig)) {
                         Serial.printf("[SEC] Sig mismatch! Exp=%s\n", calc.c_str());
                         return; // Dropped
                     }
                     // Verified
                }
                // Unwrap
                size_t dLen = strlen(dataStr);
                if (dLen >= 768) return;
                memcpy(job.payload, dataStr, dLen);
                job.payload[dLen] = 0;
                job.len = dLen;
            } else {
                if (_cfg.secret_token.length() > 0) {
                    Serial.println("[SEC] Unsigned msg dropped (claimed)");
                    return;
                }
                memcpy(job.payload, payload, length);
                job.payload[length] = 0; // Ensure null term if buffer allows, but buffer is large enough usually. Struct has 768.
                job.len = length;
            }
            if (xQueueSend(_jobQueue, &job, 0) != pdTRUE) {
                Serial.println("[MQTT] Queue full");
            }
        }
    });

    if (!mqttConnect()) {
        Serial.println("[RUN] MQTT initial connect failed");
    }
}

void SabaDevice::toolWorkerTaskWrapper(void* ctx) {
    SabaDevice* self = (SabaDevice*)ctx;
    self->toolWorkerTask();
}

void SabaDevice::toolWorkerTask() {
    Serial.println("[TOOL] Worker task started");
    String lastRids[20];
    int ridIdx = 0;

    for (;;) {
        ToolJob job;
        if (xQueueReceive(_jobQueue, &job, portMAX_DELAY) != pdTRUE) continue;

        StaticJsonDocument<768> cmd;
        if (deserializeJson(cmd, job.payload) != DeserializationError::Ok) continue;

        const char* rid = cmd["request_id"];
        if (rid) {
            bool dup = false;
            for(const auto& s : lastRids) if(s.equals(rid)) dup=true;
            if(dup) { Serial.println("Duplicate RID"); continue; }
            lastRids[ridIdx] = String(rid);
            ridIdx = (ridIdx+1)%20;
        }

        String eventsJson;
        if (!_toolRegistry.dispatch(cmd, eventsJson, _httpBase)) {
             Serial.println("[TOOL] Dispatch failed");
        } else {
             // Patch assets
             StaticJsonDocument<2048> tmp;
             if (deserializeJson(tmp, eventsJson) == DeserializationError::Ok) {
                 JsonArray assets = tmp["result"]["assets"];
                 if (!assets.isNull()) {
                     for (JsonObject a : assets) {
                         const char* u = a["url"];
                         if(u && u[0]=='/') a["url"] = _httpBase + String(u);
                     }
                     String patched; serializeJson(tmp, patched); eventsJson = patched;
                 }
             }
             mqttPublishSafe(topicEvents(_cfg.device_id), eventsJson);
        }
    }
}

// Helpers
bool SabaDevice::mqttPublishSafe(const String& topic, const String& msg, bool retain) {
    if (!_mqtt.connected()) return false;
    if (_mqttMutex) xSemaphoreTakeRecursive(_mqttMutex, portMAX_DELAY);
    bool ok = _mqtt.publish(topic.c_str(), msg.c_str(), retain);
    if (_mqttMutex) xSemaphoreGiveRecursive(_mqttMutex);
    return ok;
}

void SabaDevice::mqttLoopSafe() {
    if (_mqttMutex) xSemaphoreTakeRecursive(_mqttMutex, portMAX_DELAY);
    _mqtt.loop();
    if (_mqttMutex) xSemaphoreGiveRecursive(_mqttMutex);
}

void SabaDevice::publishAnnounce() {
    String ann = _toolRegistry.buildAnnounce(_cfg.device_id, _httpBase);
    mqttPublishSafe(topicAnnounce(_cfg.device_id), ann, true);
}
void SabaDevice::publishPortsAnnounce() {
    String ann = _mainPortRegistry.buildAnnounce(_cfg.device_id);
    mqttPublishSafe(topicPortsAnnounce(_cfg.device_id), ann, true);
}
void SabaDevice::publishStatus(bool online) {
    StaticJsonDocument<256> doc;
    doc["type"] = "device.status";
    doc["device_id"] = _cfg.device_id;
    doc["online"] = online;
    doc["uptime_ms"] = millis();
    doc["rssi"] = WiFi.RSSI();
    
    // time_t logic for 'ts'
    time_t now = time(nullptr); struct tm* t=gmtime(&now); char buf[32];
    if(t) strftime(buf,32,"%Y-%m-%dT%H:%M:%SZ",t); else strcpy(buf,"1970...");
    doc["ts"] = buf;

    String s; serializeJson(doc, s);
    mqttPublishSafe(topicStatus(_cfg.device_id), s, false);
    if(online) _lastStatusMs = millis();
}
void SabaDevice::clearRetainedMessages() {
    mqttPublishSafe(topicAnnounce(_cfg.device_id), "", true);
    mqttPublishSafe(topicStatus(_cfg.device_id), "", true);
    mqttPublishSafe(topicPortsAnnounce(_cfg.device_id), "", true);
}

bool SabaDevice::mqttConnect() {
    if (_mqtt.connected()) return true;
    _mqtt.setServer(_cfg.mqtt_host.c_str(), _cfg.mqtt_port);
    
    // Last Will
    StaticJsonDocument<256> w; w["type"]="device.status"; w["device_id"]=_cfg.device_id; w["online"]=false;
    String ws; serializeJson(w, ws);

    if (_mqttMutex) xSemaphoreTakeRecursive(_mqttMutex, portMAX_DELAY);
    bool ok = _mqtt.connect(_cfg.device_id.c_str(), nullptr, nullptr, 
        topicStatus(_cfg.device_id).c_str(), 0, true, ws.c_str());
    if(!ok) { if (_mqttMutex) xSemaphoreGiveRecursive(_mqttMutex); return false; }
    
    _mqtt.subscribe(topicCmd(_cfg.device_id).c_str());
    _mqtt.subscribe(topicPortsSet(_cfg.device_id).c_str());
    _mqtt.subscribe(("mcp/dev/"+_cfg.device_id+"/claim").c_str());
    if (_mqttMutex) xSemaphoreGiveRecursive(_mqttMutex);

    publishAnnounce();
    publishStatus(true);
    publishPortsAnnounce();
    return true;
}

String SabaDevice::macTailDeviceId() {
    uint8_t mac[6]; WiFi.macAddress(mac);
    char buf[32]; snprintf(buf, 32, "dev-%02X%02X%02X", mac[3], mac[4], mac[5]);
    return String(buf);
}

String SabaDevice::hmacSha256(const String& key, const String& payload) {
    byte hmacResult[32];
    mbedtls_md_context_t ctx;
    mbedtls_md_type_t md_type = MBEDTLS_MD_SHA256;
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(md_type), 1);
    mbedtls_md_hmac_starts(&ctx, (const unsigned char *)key.c_str(), key.length());
    mbedtls_md_hmac_update(&ctx, (const unsigned char *)payload.c_str(), payload.length());
    mbedtls_md_hmac_finish(&ctx, hmacResult);
    mbedtls_md_free(&ctx);
    String hashStr;
    for(int i=0; i<32; i++){
        if(hmacResult[i]<16) hashStr+="0";
        hashStr += String(hmacResult[i], HEX);
    }
    return hashStr;
}

void SabaDevice::applyWifiTxPower() {
    WiFi.setTxPower(WIFI_POWER_8_5dBm);
    Serial.printf("[WIFI] TX power set to %.1f dBm\n", WiFi.getTxPower() * 0.25f);
}

bool SabaDevice::publishPortData(const char* portName, float value) {
    if (!_mqtt.connected()) return false;
    StaticJsonDocument<128> doc;
    doc["port"] = portName;
    doc["value"] = value;
    doc["timestamp"] = isoNow();

    String payload;
    serializeJson(doc, payload);
    return mqttPublishSafe(topicPortsData(_cfg.device_id), payload, false);
}

bool SabaDevice::publishPortState(const char* portName, float value, bool accepted, const char* source) {
    if (!_mqtt.connected()) return false;
    StaticJsonDocument<192> doc;
    doc["port"] = portName;
    doc["value"] = value;
    doc["accepted"] = accepted;
    doc["source"] = (source && *source) ? source : "ports.set";
    doc["timestamp"] = isoNow();

    String payload;
    serializeJson(doc, payload);
    return mqttPublishSafe(topicPortsState(_cfg.device_id), payload, false);
}

void SabaDevice::publishEvent(const String& eventName, const JsonObject& data) {
    // Basic impl
    StaticJsonDocument<512> doc;
    doc["type"] = "tool.event";
    doc["event"] = eventName;
    doc["data"] = data;
    String s; serializeJson(doc, s);
    mqttPublishSafe(topicEvents(_cfg.device_id), s, false);
}
