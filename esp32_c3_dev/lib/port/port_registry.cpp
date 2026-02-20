// port_registry.cpp
#include "port_registry.h"

class CallbackOutPort : public OutPort {
public:
  CallbackOutPort(
    const String& name,
    const String& dataType,
    const String& description,
    uint32_t periodMs,
    float (*readFn)()
  )
  : _name(name), _dataType(dataType), _description(description),
    _periodMs(periodMs), _readFn(readFn), _lastMs(0) {}

  const char* name() const override { return _name.c_str(); }

  void describe(JsonObject& port) override {
    port["name"] = _name;
    port["type"] = "outport";
    port["data_type"] = _dataType;
    port["description"] = _description;
  }

  uint32_t periodMs() const override { return _periodMs; }

  void tick(uint32_t now_ms) override {
    if (!_readFn) return;
    if (_lastMs != 0 && (now_ms - _lastMs) < _periodMs) return;
    _lastMs = now_ms;
    float v = _readFn();
    port_publish_data(_name.c_str(), v);
  }

private:
  String _name;
  String _dataType;
  String _description;
  uint32_t _periodMs;
  float (*_readFn)();
  uint32_t _lastMs;
};

void PortRegistry::addOutPort(
  const String& name,
  const String& dataType,
  const String& description,
  uint32_t periodMs,
  float (*readFn)()
) {
  CallbackOutPort* p = new CallbackOutPort(name, dataType, description, periodMs, readFn);
  owned_outports.push_back(p);
  outports.push_back(p);
}

void PortRegistry::createInPort(const String& name, const String& type) {
  InPort p;
  p.name     = name;
  p.dataType = type;
  p.value    = 0.0f;
  inports.push_back(p);
}

InPort* PortRegistry::findInPort(const String& name) {
  for (auto& p : inports) {
    if (p.name == name) return &p;
  }
  return nullptr;
}

void PortRegistry::handleInPortSet(const String& name, float value) {
  InPort* p = findInPort(name);
  if (!p) {
    Serial.printf("[PORT] InPort '%s' not found\n", name.c_str());
    return;
  }
  p->value = value;
  Serial.printf("[PORT] InPort '%s' set to %.3f\n", name.c_str(), value);
}

float PortRegistry::getInPortValue(const char* name, float defaultValue) {
  if (!name || !*name) return defaultValue;
  InPort* p = findInPort(String(name));
  if (!p) return defaultValue;
  return p->value;
}

void PortRegistry::tickAll(uint32_t now_ms) {
  (void)now_ms;
  for (auto* p : outports) {
    if (!p) continue;
    p->tick(now_ms);
  }
}

String PortRegistry::buildAnnounce(const String& device_id) const {
  StaticJsonDocument<1024> doc;
  doc["type"]      = "ports.announce";
  doc["device_id"] = device_id;

  // timestamp (optional)
  time_t now = time(nullptr);
  struct tm* t = gmtime(&now);
  char buf[32];
  if (t) strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", t);
  else   snprintf(buf, sizeof(buf), "1970-01-01T00:00:00Z");
  doc["timestamp"] = buf;

  JsonArray outArr = doc.createNestedArray("outports");
  for (auto* p : outports) {
    if (!p) continue;
    JsonObject o = outArr.createNestedObject();
    p->describe(o);
  }

  JsonArray inArr = doc.createNestedArray("inports");
  for (auto& ip : inports) {
    JsonObject o = inArr.createNestedObject();
    ip.describe(o);
  }

  String s;
  serializeJson(doc, s);
  return s;
}

// 기본 구현: 아무 포트도 등록하지 않음.
// 네가 modules/xxx_ports.cpp 에서 이 함수를 또 정의하면
// 그쪽이 링크 단계에서 사용됨(이 파일은 프로젝트에서 빼도 됨).
// lib/port/port_registry.cpp

__attribute__((weak))
void register_ports(PortRegistry& reg, const PortConfig& cfg) {
  (void)reg;
  (void)cfg;
  // 기본은 아무것도 안 함
}

extern PortRegistry g_portRegistry;

float port_get_inport_value(const char* name) {
  return g_portRegistry.getInPortValue(name, NAN);
}

