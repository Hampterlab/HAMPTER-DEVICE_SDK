// port_registry.cpp
#include "port_registry.h"

namespace {

void describeRange(JsonObject& parent, const char* field, const PortRange& range) {
  if (!range.enabled) return;
  JsonObject obj = parent.createNestedObject(field);
  if (!isnan(range.min)) obj["min"] = range.min;
  if (!isnan(range.max)) obj["max"] = range.max;
}

void describeValueSpec(JsonObject& port, const PortValueSpec& spec) {
  port["description"] = spec.description.length() ? spec.description : "General-purpose variable slot";
  if (spec.unit.length()) {
    port["unit"] = spec.unit;
  }
  if (spec.hasDefaultValue) {
    port["default_value"] = spec.defaultValue;
  }
  if (spec.hasStep && !isnan(spec.step)) {
    port["step"] = spec.step;
  }
  describeRange(port, "expected_range", spec.expectedRange);
  describeRange(port, "hard_limits", spec.hardLimits);
  if (spec.outOfRangePolicy.length()) {
    port["out_of_range_policy"] = spec.outOfRangePolicy;
  }
}

const char* extractPortNameFromTarget(const char* target) {
  if (!target || !*target) return nullptr;
  const char* slash = strrchr(target, '/');
  return slash ? (slash + 1) : target;
}

bool extractPortSetObject(JsonObjectConst obj, String& outName, float& outValue) {
  const char* directName = nullptr;
  if (obj["port"].is<const char*>()) {
    directName = obj["port"].as<const char*>();
  }
  if ((!directName || !*directName) && obj["port_name"].is<const char*>()) {
    directName = obj["port_name"].as<const char*>();
  }
  if ((!directName || !*directName) && obj["name"].is<const char*>()) {
    directName = obj["name"].as<const char*>();
  }
  if (!directName) {
    const char* target = nullptr;
    if (obj["target"].is<const char*>()) {
      target = obj["target"].as<const char*>();
    }
    directName = extractPortNameFromTarget(target);
  }

  if (directName && *directName) {
    outName = String(directName);
    if (!obj["value"].isNull()) {
      outValue = obj["value"].as<float>();
    }
    return true;
  }

  JsonVariantConst dataVar = obj["data"];
  if (dataVar.is<JsonObjectConst>()) {
    return extractPortSetObject(dataVar.as<JsonObjectConst>(), outName, outValue);
  }

  if (dataVar.is<const char*>()) {
    StaticJsonDocument<256> nested;
    if (deserializeJson(nested, dataVar.as<const char*>()) == DeserializationError::Ok) {
      return extractPortSetObject(nested.as<JsonObjectConst>(), outName, outValue);
    }
  }

  return false;
}

}  // namespace

class CallbackOutPort : public OutPort {
public:
  CallbackOutPort(
    const String& name,
    const String& dataType,
    uint32_t periodMs,
    float (*readFn)(),
    const PortValueSpec& spec
  )
  : _name(name), _dataType(dataType), _periodMs(periodMs),
    _readFn(readFn), _lastMs(0), _spec(spec) {}

  const char* name() const override { return _name.c_str(); }

  void describe(JsonObject& port) override {
    port["name"] = _name;
    port["type"] = "outport";
    port["data_type"] = _dataType;
    describeValueSpec(port, _spec);
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
  uint32_t _periodMs;
  float (*_readFn)();
  uint32_t _lastMs;
  PortValueSpec _spec;
};

void PortRegistry::addOutPort(
  const String& name,
  const String& dataType,
  const String& description,
  uint32_t periodMs,
  float (*readFn)()
) {
  PortValueSpec spec;
  spec.description = description;
  CallbackOutPort* p = new CallbackOutPort(name, dataType, periodMs, readFn, spec);
  owned_outports.push_back(p);
  outports.push_back(p);
}

void PortRegistry::addOutPort(
  const String& name,
  const String& dataType,
  uint32_t periodMs,
  float (*readFn)(),
  const PortValueSpec& spec
) {
  CallbackOutPort* p = new CallbackOutPort(name, dataType, periodMs, readFn, spec);
  owned_outports.push_back(p);
  outports.push_back(p);
}

void PortRegistry::createInPort(
  const String& name,
  const String& type,
  const PortValueSpec& spec
) {
  InPort* existing = findInPort(name);
  if (existing) {
    existing->dataType = type;
    existing->spec = spec;
    if (!existing->hasValue) {
      existing->value = spec.hasDefaultValue ? spec.defaultValue : 0.0f;
      existing->hasValue = spec.hasDefaultValue;
    }
    return;
  }

  InPort p;
  p.name = name;
  p.dataType = type;
  p.spec = spec;
  p.value = spec.hasDefaultValue ? spec.defaultValue : 0.0f;
  p.hasValue = spec.hasDefaultValue;
  inports.push_back(p);
}

void PortRegistry::createInPort(
  const String& name,
  const String& type,
  const String& description,
  float defaultValue,
  void (*onSet)(float value)
) {
  PortValueSpec spec;
  spec.description = description;
  spec.hasDefaultValue = true;
  spec.defaultValue = defaultValue;
  spec.onSet = onSet;
  createInPort(name, type, spec);
}

InPort* PortRegistry::findInPort(const String& name) {
  for (auto& p : inports) {
    if (p.name == name) return &p;
  }
  return nullptr;
}

bool PortRegistry::handleInPortSet(const String& name, float value, const char* source, bool publishState) {
  InPort* p = findInPort(name);
  if (!p) {
    Serial.printf("[PORT] InPort '%s' not found\n", name.c_str());
    if (publishState) {
      port_publish_state(name.c_str(), value, false, source);
    }
    return false;
  }
  float appliedValue = value;
  const PortRange& limits = p->spec.hardLimits;
  const bool belowMin = limits.enabled && !isnan(limits.min) && appliedValue < limits.min;
  const bool aboveMax = limits.enabled && !isnan(limits.max) && appliedValue > limits.max;
  const bool outsideLimits = belowMin || aboveMax;
  String policy = p->spec.outOfRangePolicy.length() ? p->spec.outOfRangePolicy : "allow";
  policy.toLowerCase();

  if (outsideLimits) {
    if (policy == "reject") {
      Serial.printf("[PORT] InPort '%s' rejected %.3f by hard limits\n", name.c_str(), value);
      if (publishState) {
        port_publish_state(name.c_str(), value, false, source);
      }
      return false;
    }
    if (policy == "clamp") {
      if (!isnan(limits.min) && appliedValue < limits.min) appliedValue = limits.min;
      if (!isnan(limits.max) && appliedValue > limits.max) appliedValue = limits.max;
    }
  }

  p->value = appliedValue;
  p->hasValue = true;
  if (p->spec.onSet) {
    p->spec.onSet(appliedValue);
  }
  Serial.printf("[PORT] InPort '%s' set to %.3f\n", name.c_str(), appliedValue);
  if (publishState) {
    port_publish_state(name.c_str(), appliedValue, true, source);
  }
  return true;
}

bool PortRegistry::parseInPortSetPayload(
  const byte* payload,
  unsigned length,
  String& outName,
  float& outValue,
  String* outRawJson
) const {
  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err) {
    Serial.printf("[PORT] ports.set JSON parse error: %s\n", err.c_str());
    return false;
  }

  if (outRawJson) {
    outRawJson->remove(0);
    serializeJson(doc, *outRawJson);
  }

  outName = "";
  outValue = doc["value"] | 0.0f;
  if (extractPortSetObject(doc.as<JsonObjectConst>(), outName, outValue)) {
    return true;
  }

  if (outRawJson && outRawJson->length() > 0) {
    Serial.printf("[PORT] ports.set missing port field payload=%s\n", outRawJson->c_str());
  } else {
    Serial.println("[PORT] ports.set missing port field");
  }
  return false;
}

float PortRegistry::getInPortValue(const char* name, float defaultValue) {
  if (!name || !*name) return defaultValue;
  InPort* p = findInPort(String(name));
  if (!p) return defaultValue;
  if (!p->hasValue && p->spec.hasDefaultValue) return p->spec.defaultValue;
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
    describeValueSpec(o, ip.spec);
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

