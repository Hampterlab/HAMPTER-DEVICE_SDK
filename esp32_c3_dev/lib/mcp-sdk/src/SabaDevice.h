#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <freertos/semphr.h>

#include "registry.h"
#include "port_registry.h"
#include "provisioning_service.h"

// Configuration structure
struct SabaConfig {
    const char* deviceName;
    const char* fwVersion;
    uint16_t httpPort = 80;
};

// Global config object found in main.cpp, we might need to encapsulate it or make it passable.
// McpConfig is defined in provisioning_service.h which is included above.

class SabaDevice {
public:
    SabaDevice(const char* deviceName, const char* fwVersion);
    ~SabaDevice();

    void begin();
    void loop();

    // Accessors for registration
    ToolRegistry& getToolRegistry() { return _toolRegistry; }
    PortRegistry& getPortRegistry() { return _mainPortRegistry; } // Renamed to avoid confusion with global? NO, use member, refer global in cpp.

    // Helpers exposed
    bool publishPortData(const char* portName, float value);
    bool publishPortState(const char* portName, float value, bool accepted, const char* source = nullptr);
    void publishEvent(const String& eventName, const JsonObject& data);

private:
    // Config
    String _deviceName;
    String _fwVersion;
    
    // Core Components
    WebServer        _server;
    DNSServer        _dnsServer;
    Preferences      _prefs;
    WiFiClient       _wifiClient;
    PubSubClient     _mqtt;
    
    ToolRegistry     _toolRegistry;
    // We use a reference to the global one, or just the member?
    // Since g_portRegistry is extern, we can just use the global one and not have a member, 
    // OR have a member and bind GLOBAL to it.
    // Let's use reference.
    PortRegistry&    _mainPortRegistry;
    ProvisioningService* _provisioning = nullptr;

    // State
    McpConfig        _cfg;
    bool             _isProvisionMode = false;
    String           _httpBase;
    
    // Timing
    unsigned long    _lastStatusMs = 0;
    unsigned long    _lastAnnounceMs = 0;
    unsigned long    _lastMqttTry = 0;
    unsigned long    _lastWifiTry = 0;
    unsigned long    _lastWifiDbg = 0;

    // RTOS
    QueueHandle_t     _jobQueue = nullptr;
    SemaphoreHandle_t _mqttMutex = nullptr;

    // Internal Methods
    void setupHttpHandlers();
    void startProvisioning();
    void startRuntime();
    void handleRuntime();

    // MQTT Internals
    bool mqttConnect();
    void mqttLoopSafe();
    bool mqttPublishSafe(const String& topic, const String& msg, bool retain=false);
    
    void publishAnnounce();
    void publishStatus(bool online);
    void publishPortsAnnounce();
    void clearRetainedMessages();

    // Helper
    String macTailDeviceId();
    String hmacSha256(const String& key, const String& payload);
    void applyWifiTxPower();

    // Static Task Wrapper
    static void toolWorkerTaskWrapper(void* ctx);
    void toolWorkerTask();
};
