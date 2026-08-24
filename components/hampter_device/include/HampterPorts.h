#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <math.h>

namespace hampter::internal {
class Runtime;
}

struct PortRange {
  bool enabled = false;
  float min = NAN;
  float max = NAN;

  static PortRange bounded(float minimum, float maximum) {
    PortRange range;
    range.enabled = true;
    range.min = minimum;
    range.max = maximum;
    return range;
  }
};

// Registration-time value object retained from the MQTT SDK. PortRegistry
// copies it into compact POD metadata; none of these Strings live in the
// active runtime store.
struct PortValueSpec {
  String description;
  String unit;
  bool hasDefaultValue = false;
  float defaultValue = 0.0f;
  bool hasStep = false;
  float step = NAN;
  PortRange expectedRange;
  PortRange hardLimits;
  String outOfRangePolicy = "allow";
  void (*onSet)(float value) = nullptr;
};

PortValueSpec makeInputSpec(const char* description, float expectedMin,
                            float expectedMax, float defaultValue = 0.0f,
                            const char* unit = nullptr,
                            const char* outOfRangePolicy = "allow");
PortValueSpec makeOutputSpec(const char* description, const char* unit,
                             float expectedMin, float expectedMax,
                             float step = NAN);

class OutPort {
 public:
  virtual ~OutPort() = default;
  virtual const char* name() const = 0;
  virtual void describe(JsonObject& port) = 0;
  virtual uint32_t periodMs() const = 0;
  virtual void tick(uint32_t nowMs) = 0;
};

// Lightweight compatibility view. Validation metadata is stored once in the
// registry entry instead of duplicating heap-backed Strings per InPort.
struct InPort {
  const char* name = "";
  const char* dataType = "float";
  float value = 0.0f;
  bool hasValue = false;
};

class PortRegistry {
 public:
  static constexpr size_t kCapacity = CONFIG_HAMPTER_MAX_PORTS;

  void addOutPort(OutPort* port);
  void addOutPort(const String& name, const String& dataType,
                  uint32_t periodMs, float (*readFn)(),
                  const PortValueSpec& spec = PortValueSpec{});
  void addOutPort(const String& name, const String& dataType,
                  const String& description, uint32_t periodMs,
                  float (*readFn)());
  void addOutPort(const String& name, const String& description,
                  const String& unit, float expectedMin, float expectedMax,
                  uint32_t periodMs, float (*readFn)(), float step = NAN);

  void createInPort(const String& name, const String& type,
                    const PortValueSpec& spec = PortValueSpec{});
  void createInPort(const String& name, const String& type,
                    const String& description, float defaultValue = 0.0f,
                    void (*onSet)(float value) = nullptr);

  void addInPort(const String& name, const String& type = "float",
                 const PortValueSpec& spec = PortValueSpec{}) {
    createInPort(name, type, spec);
  }
  void addInPort(const String& name, const String& type,
                 const String& description, float defaultValue = 0.0f,
                 void (*onSet)(float value) = nullptr) {
    createInPort(name, type, description, defaultValue, onSet);
  }
  void addInPort(const String& name, const String& description,
                 float expectedMin, float expectedMax,
                 float defaultValue = 0.0f, const String& unit = "",
                 const String& outOfRangePolicy = "allow") {
    addInPort(name, "float",
              makeInputSpec(description.c_str(), expectedMin, expectedMax,
                            defaultValue,
                            unit.length() ? unit.c_str() : nullptr,
                            outOfRangePolicy.c_str()));
  }

  size_t outportCount() const { return outCount_; }
  size_t inportCount() const { return inCount_; }
  bool valid() const { return error_ == nullptr; }
  const char* error() const { return error_; }

  InPort* findInPort(const String& name);
  bool handleInPortSet(const String& name, float value);
  float getInPortValue(const char* name, float defaultValue = NAN) const;
  void tickAll(uint32_t nowMs);
  uint32_t millisecondsUntilNextTick(uint32_t nowMs,
                                     uint32_t maximumMs) const;

 private:
  friend class hampter::internal::Runtime;

  enum class Direction : uint8_t { In, Out };
  enum class RangePolicy : uint8_t { Allow, Clamp, Reject };

  struct Entry {
    const char* name = "";
    const char* dataType = "float";
    const char* description = "";
    const char* unit = "";
    Direction direction = Direction::In;
    RangePolicy policy = RangePolicy::Allow;
    OutPort* customOut = nullptr;
    float (*readFn)() = nullptr;
    void (*onSet)(float) = nullptr;
    uint32_t periodMs = 0;
    uint32_t lastReadMs = 0;
    float expectedMin = NAN;
    float expectedMax = NAN;
    float hardMin = NAN;
    float hardMax = NAN;
    float step = NAN;
    bool expectedRange = false;
    bool hardLimits = false;
    bool hasStep = false;
    bool hasRead = false;
    InPort in;
  };

  const char* copyText(const char* text);
  bool addEntry(const String& name, const String& dataType,
                Direction direction, const PortValueSpec& spec,
                Entry*& output);
  int findEntry(const char* name, Direction direction) const;
  bool applyInbound(size_t index, float value);
  size_t totalCount() const { return count_; }
  const Entry* entry(size_t index) const {
    return index < count_ ? &entries_[index] : nullptr;
  }

  Entry entries_[kCapacity]{};
  char metadata_[CONFIG_HAMPTER_METADATA_POOL_BYTES]{};
  size_t metadataUsed_ = 0;
  size_t count_ = 0;
  size_t outCount_ = 0;
  size_t inCount_ = 0;
  const char* error_ = nullptr;
};

bool port_publish_data(const char* portName, float value);
void port_set_outport_value(const char* portName, float value);
float port_get_inport_value(const char* name);
