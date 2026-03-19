// port_registry.h
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <vector>
#include <math.h>

struct PortRange {
  bool enabled = false;
  float min = NAN;
  float max = NAN;

  static PortRange bounded(float minValue, float maxValue) {
    PortRange range;
    range.enabled = true;
    range.min = minValue;
    range.max = maxValue;
    return range;
  }
};

struct PortValueSpec {
  String description = "";
  String unit = "";
  bool hasDefaultValue = false;
  float defaultValue = 0.0f;
  bool hasStep = false;
  float step = NAN;
  PortRange expectedRange;
  PortRange hardLimits;
  String outOfRangePolicy = "allow";
  void (*onSet)(float value) = nullptr;
};

inline PortValueSpec makeInputSpec(
  const char* description,
  float expectedMin,
  float expectedMax,
  float defaultValue = 0.0f,
  const char* unit = nullptr,
  const char* outOfRangePolicy = "allow"
) {
  PortValueSpec spec;
  spec.description = description ? description : "";
  spec.hasDefaultValue = true;
  spec.defaultValue = defaultValue;
  spec.expectedRange = PortRange::bounded(expectedMin, expectedMax);
  if (unit) spec.unit = unit;
  if (outOfRangePolicy) spec.outOfRangePolicy = outOfRangePolicy;
  return spec;
}

inline PortValueSpec makeOutputSpec(
  const char* description,
  const char* unit,
  float expectedMin,
  float expectedMax,
  float step = NAN
) {
  PortValueSpec spec;
  spec.description = description ? description : "";
  if (unit) spec.unit = unit;
  spec.expectedRange = PortRange::bounded(expectedMin, expectedMax);
  if (!isnan(step)) {
    spec.hasStep = true;
    spec.step = step;
  }
  return spec;
}

// ===== OutPort (센서/데이터 소스) =====
class OutPort {
public:
  virtual ~OutPort() {}
  virtual const char* name() const = 0;
  virtual void describe(JsonObject& port) = 0;
  virtual uint32_t periodMs() const = 0;      // tick 주기 (ms)
  virtual void tick(uint32_t now_ms) = 0;     // PortRegistry/Task에서 주기적으로 호출
};

// ===== InPort (범용 변수 슬롯) =====
struct InPort {
  String name;      // "var_a"
  String dataType;  // "float", "bool" 등
  PortValueSpec spec;
  float  value;     // 단순화: float 하나
  bool   hasValue = false;

  void describe(JsonObject& port) const {
    port["name"]        = name;
    port["type"]        = "inport";
    port["data_type"]   = dataType;
  }
};

// ===== PortRegistry =====
class PortRegistry {
public:
  // OutPort 관리
  void addOutPort(OutPort* p) { outports.push_back(p); }
  void addOutPort(
    const String& name,
    const String& dataType,
    const String& description,
    uint32_t periodMs,
    float (*readFn)()
  );
  void addOutPort(
    const String& name,
    const String& dataType,
    uint32_t periodMs,
    float (*readFn)(),
    const PortValueSpec& spec
  );
  size_t outportCount() const { return outports.size(); }

  // InPort 관리
  void createInPort(
    const String& name,
    const String& type,
    const PortValueSpec& spec = PortValueSpec()
  );
  void createInPort(
    const String& name,
    const String& type,
    const String& description,
    float defaultValue = 0.0f,
    void (*onSet)(float value) = nullptr
  );
  void addInPort(
    const String& name,
    const String& type = "float",
    const PortValueSpec& spec = PortValueSpec()
  ) { createInPort(name, type, spec); }
  void addInPort(
    const String& name,
    const String& type,
    const String& description,
    float defaultValue = 0.0f,
    void (*onSet)(float value) = nullptr
  ) { createInPort(name, type, description, defaultValue, onSet); }
  void addInPort(
    const String& name,
    const String& description,
    float expectedMin,
    float expectedMax,
    float defaultValue = 0.0f,
    const String& unit = "",
    const String& outOfRangePolicy = "allow"
  ) {
    addInPort(
      name,
      "float",
      makeInputSpec(
        description.c_str(),
        expectedMin,
        expectedMax,
        defaultValue,
        unit.length() ? unit.c_str() : nullptr,
        outOfRangePolicy.c_str()
      )
    );
  }
  void addOutPort(
    const String& name,
    const String& description,
    const String& unit,
    float expectedMin,
    float expectedMax,
    uint32_t periodMs,
    float (*readFn)(),
    float step = NAN
  ) {
    addOutPort(
      name,
      "float",
      periodMs,
      readFn,
      makeOutputSpec(
        description.c_str(),
        unit.length() ? unit.c_str() : nullptr,
        expectedMin,
        expectedMax,
        step
      )
    );
  }
  InPort* findInPort(const String& name);
  bool handleInPortSet(const String& name, float value, const char* source = "ports.set", bool publishState = true);
  bool parseInPortSetPayload(const byte* payload, unsigned length, String& outName, float& outValue, String* outRawJson = nullptr) const;
  float getInPortValue(const char* name, float defaultValue = NAN);
  size_t inportCount() const { return inports.size(); }

  // 주기 tick
  void tickAll(uint32_t now_ms);

  // ports.announce payload 생성
  String buildAnnounce(const String& device_id) const;

private:
  std::vector<OutPort*> outports;
  std::vector<OutPort*> owned_outports;
  std::vector<InPort>   inports;
};

// ===== 포트 설정용 Config & Hook =====
struct PortConfig { int dummy = 0; };

// ★ 여기가 툴의 register_tools()와 같은 포트용 확장 훅
void register_ports(PortRegistry& reg, const PortConfig& cfg);

// ===== OutPort에서 쓰는 헬퍼 (main.cpp에서 구현) =====
// OutPort::tick 안에서 이렇게 사용: port_publish_data(name(), value);
bool port_publish_data(const char* portName, float value);
bool port_publish_state(const char* portName, float value, bool accepted, const char* source = nullptr);
void port_set_outport_value(const char* portName, float value);

// ===== Formula helper =====
// Dynamic pattern modules can read InPort values through this function.
float port_get_inport_value(const char* name);
