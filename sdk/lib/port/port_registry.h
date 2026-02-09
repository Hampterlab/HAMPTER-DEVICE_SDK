// port_registry.h
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <vector>

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
  float  value;     // 단순화: float 하나

  void describe(JsonObject& port) const {
    port["name"]        = name;
    port["type"]        = "inport";
    port["data_type"]   = dataType;
    port["description"] = "General-purpose variable slot";
  }
};

// ===== PortRegistry =====
class PortRegistry {
public:
  // OutPort 관리
  void addOutPort(OutPort* p) { outports.push_back(p); }
  size_t outportCount() const { return outports.size(); }

  // InPort 관리
  void createInPort(const String& name, const String& type);
  InPort* findInPort(const String& name);
  void handleInPortSet(const String& name, float value);
  size_t inportCount() const { return inports.size(); }

  // 주기 tick
  void tickAll(uint32_t now_ms);

  // ports.announce payload 생성
  String buildAnnounce(const String& device_id) const;

private:
  std::vector<OutPort*> outports;
  std::vector<InPort>   inports;
};

// ===== 포트 설정용 Config & Hook =====
struct PortConfig { int dummy = 0; };

// ★ 여기가 툴의 register_tools()와 같은 포트용 확장 훅
void register_ports(PortRegistry& reg, const PortConfig& cfg);

// ===== OutPort에서 쓰는 헬퍼 (main.cpp에서 구현) =====
// OutPort::tick 안에서 이렇게 사용: port_publish_data(name(), value);
bool port_publish_data(const char* portName, float value);
