#include "HampterPorts.h"

#include <string.h>

PortValueSpec makeInputSpec(const char* description, float expectedMin,
                            float expectedMax, float defaultValue,
                            const char* unit,
                            const char* outOfRangePolicy) {
  PortValueSpec spec;
  spec.description = description ? description : "";
  spec.unit = unit ? unit : "";
  spec.hasDefaultValue = true;
  spec.defaultValue = defaultValue;
  spec.expectedRange = PortRange::bounded(expectedMin, expectedMax);
  spec.outOfRangePolicy = outOfRangePolicy ? outOfRangePolicy : "allow";
  return spec;
}

PortValueSpec makeOutputSpec(const char* description, const char* unit,
                             float expectedMin, float expectedMax,
                             float step) {
  PortValueSpec spec;
  spec.description = description ? description : "";
  spec.unit = unit ? unit : "";
  spec.expectedRange = PortRange::bounded(expectedMin, expectedMax);
  if (!isnan(step)) {
    spec.hasStep = true;
    spec.step = step;
  }
  return spec;
}

const char* PortRegistry::copyText(const char* text) {
  if (text == nullptr) text = "";
  const size_t length = strlen(text) + 1;
  if (metadataUsed_ + length > sizeof(metadata_)) {
    error_ = "Port metadata arena exhausted";
    return nullptr;
  }
  char* destination = metadata_ + metadataUsed_;
  memcpy(destination, text, length);
  metadataUsed_ += length;
  return destination;
}

bool PortRegistry::addEntry(const String& name, const String& dataType,
                            Direction direction, const PortValueSpec& spec,
                            Entry*& output) {
  output = nullptr;
  if (name.isEmpty()) {
    error_ = "Port name is empty";
    return false;
  }
  if (dataType != "float") {
    error_ = "unsupported Port data type (only float is supported)";
    return false;
  }
  if (count_ >= kCapacity) {
    error_ = "total Port registry capacity exceeded";
    return false;
  }
  if (findEntry(name.c_str(), Direction::In) >= 0 ||
      findEntry(name.c_str(), Direction::Out) >= 0) {
    error_ = "duplicate Port name";
    return false;
  }

  const bool validExpected =
      !spec.expectedRange.enabled ||
      (isfinite(spec.expectedRange.min) && isfinite(spec.expectedRange.max) &&
       spec.expectedRange.min <= spec.expectedRange.max);
  const bool validHard =
      !spec.hardLimits.enabled ||
      (isfinite(spec.hardLimits.min) && isfinite(spec.hardLimits.max) &&
       spec.hardLimits.min <= spec.hardLimits.max);
  if (!validExpected || !validHard ||
      (spec.hasDefaultValue && !isfinite(spec.defaultValue)) ||
      (spec.hasStep && (!isfinite(spec.step) || spec.step <= 0.0f))) {
    error_ = "invalid Port range, default, or step";
    return false;
  }

  RangePolicy policy = RangePolicy::Allow;
  if (spec.outOfRangePolicy == "clamp") {
    policy = RangePolicy::Clamp;
  } else if (spec.outOfRangePolicy == "reject") {
    policy = RangePolicy::Reject;
  } else if (spec.outOfRangePolicy != "allow") {
    error_ = "unknown Port out-of-range policy";
    return false;
  }
  const bool restrictToExpected =
      !spec.hardLimits.enabled && spec.expectedRange.enabled &&
      policy != RangePolicy::Allow;
  const bool effectiveHardLimits =
      spec.hardLimits.enabled || restrictToExpected;
  const float effectiveHardMin =
      spec.hardLimits.enabled ? spec.hardLimits.min : spec.expectedRange.min;
  const float effectiveHardMax =
      spec.hardLimits.enabled ? spec.hardLimits.max : spec.expectedRange.max;
  if (direction == Direction::In && spec.hasDefaultValue &&
      effectiveHardLimits && policy == RangePolicy::Reject &&
      (spec.defaultValue < effectiveHardMin ||
       spec.defaultValue > effectiveHardMax)) {
    error_ = "InPort default is outside its rejected range";
    return false;
  }

  Entry& item = entries_[count_];
  item.name = copyText(name.c_str());
  item.dataType = copyText(dataType.length() ? dataType.c_str() : "float");
  if (item.name == nullptr || item.dataType == nullptr) return false;

  if (spec.description.length()) {
    item.description = copyText(spec.description.c_str());
  } else {
    char fallback[112];
    snprintf(fallback, sizeof(fallback), "%s %s value", name.c_str(),
             direction == Direction::In ? "input" : "output");
    item.description = copyText(fallback);
  }
  item.unit = copyText(spec.unit.c_str());
  if (item.description == nullptr || item.unit == nullptr) return false;

  item.direction = direction;
  item.onSet = spec.onSet;
  item.expectedRange = spec.expectedRange.enabled;
  item.expectedMin = spec.expectedRange.min;
  item.expectedMax = spec.expectedRange.max;
  item.hardLimits = effectiveHardLimits;
  item.hardMin = effectiveHardMin;
  item.hardMax = effectiveHardMax;
  item.hasStep = spec.hasStep;
  item.step = spec.step;
  item.policy = policy;
  item.in.name = item.name;
  item.in.dataType = item.dataType;
  if (direction == Direction::In && spec.hasDefaultValue) {
    item.in.value =
        effectiveHardLimits && policy == RangePolicy::Clamp
            ? constrain(spec.defaultValue, effectiveHardMin, effectiveHardMax)
            : spec.defaultValue;
    item.in.hasValue = true;
  }

  output = &item;
  ++count_;
  if (direction == Direction::In) {
    ++inCount_;
  } else {
    ++outCount_;
  }
  return true;
}

void PortRegistry::addOutPort(OutPort* port) {
  if (port == nullptr || port->name() == nullptr || port->name()[0] == '\0') {
    error_ = "invalid custom OutPort";
    return;
  }
  PortValueSpec spec;
  Entry* item = nullptr;
  if (!addEntry(String(port->name()), "float", Direction::Out, spec, item)) {
    return;
  }
  item->customOut = port;
  item->periodMs = port->periodMs();
}

void PortRegistry::addOutPort(const String& name, const String& dataType,
                              uint32_t periodMs, float (*readFn)(),
                              const PortValueSpec& spec) {
  if (readFn == nullptr) {
    error_ = "function OutPort read callback is null";
    return;
  }
  Entry* item = nullptr;
  if (!addEntry(name, dataType, Direction::Out, spec, item)) return;
  item->periodMs = periodMs;
  item->readFn = readFn;
}

void PortRegistry::addOutPort(const String& name, const String& dataType,
                              const String& description, uint32_t periodMs,
                              float (*readFn)()) {
  PortValueSpec spec;
  spec.description = description;
  addOutPort(name, dataType, periodMs, readFn, spec);
}

void PortRegistry::addOutPort(const String& name, const String& description,
                              const String& unit, float expectedMin,
                              float expectedMax, uint32_t periodMs,
                              float (*readFn)(), float step) {
  addOutPort(name, "float", periodMs, readFn,
             makeOutputSpec(description.c_str(),
                            unit.length() ? unit.c_str() : nullptr,
                            expectedMin, expectedMax, step));
}

void PortRegistry::createInPort(const String& name, const String& type,
                                const PortValueSpec& spec) {
  Entry* ignored = nullptr;
  (void)addEntry(name, type, Direction::In, spec, ignored);
}

void PortRegistry::createInPort(const String& name, const String& type,
                                const String& description,
                                float defaultValue,
                                void (*onSet)(float value)) {
  PortValueSpec spec;
  spec.description = description;
  spec.hasDefaultValue = true;
  spec.defaultValue = defaultValue;
  spec.onSet = onSet;
  createInPort(name, type, spec);
}

int PortRegistry::findEntry(const char* name, Direction direction) const {
  if (name == nullptr) return -1;
  for (size_t i = 0; i < count_; ++i) {
    if (entries_[i].direction == direction &&
        strcmp(entries_[i].name, name) == 0) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

InPort* PortRegistry::findInPort(const String& name) {
  const int index = findEntry(name.c_str(), Direction::In);
  return index >= 0 ? &entries_[index].in : nullptr;
}

bool PortRegistry::applyInbound(size_t index, float value) {
  if (index >= count_ || !isfinite(value)) return false;
  Entry& item = entries_[index];
  if (item.direction != Direction::In) return false;

  if (item.hardLimits && (value < item.hardMin || value > item.hardMax)) {
    if (item.policy == RangePolicy::Clamp) {
      value = constrain(value, item.hardMin, item.hardMax);
    } else if (item.policy == RangePolicy::Reject) {
      return false;
    }
  }
  item.in.value = value;
  item.in.hasValue = true;
  if (item.onSet) item.onSet(value);
  return true;
}

bool PortRegistry::handleInPortSet(const String& name, float value) {
  const int index = findEntry(name.c_str(), Direction::In);
  return index >= 0 && applyInbound(static_cast<size_t>(index), value);
}

float PortRegistry::getInPortValue(const char* name, float defaultValue) const {
  const int index = findEntry(name, Direction::In);
  if (index < 0) return defaultValue;
  const InPort& port = entries_[index].in;
  return port.hasValue ? port.value : defaultValue;
}

void PortRegistry::tickAll(uint32_t nowMs) {
  for (size_t i = 0; i < count_; ++i) {
    Entry& item = entries_[i];
    if (item.direction != Direction::Out) continue;
    if (item.hasRead && item.periodMs != 0 &&
        static_cast<uint32_t>(nowMs - item.lastReadMs) < item.periodMs) {
      continue;
    }
    item.hasRead = true;
    item.lastReadMs = nowMs;
    if (item.customOut != nullptr) {
      item.customOut->tick(nowMs);
    } else if (item.readFn != nullptr) {
      const float value = item.readFn();
      if (isfinite(value)) (void)port_publish_data(item.name, value);
    }
  }
}

uint32_t PortRegistry::millisecondsUntilNextTick(
    uint32_t nowMs, uint32_t maximumMs) const {
  uint32_t waitMs = maximumMs;
  for (size_t i = 0; i < count_; ++i) {
    const Entry& item = entries_[i];
    if (item.direction != Direction::Out ||
        (item.customOut == nullptr && item.readFn == nullptr)) {
      continue;
    }
    if (!item.hasRead) return 0;
    // A zero period deliberately means every application loop. Keep the old
    // 1 ms cadence for that explicit high-rate contract instead of silently
    // turning it into the SDK's low-power default cadence.
    if (item.periodMs == 0) return waitMs < 1 ? waitMs : 1;
    const uint32_t elapsed = nowMs - item.lastReadMs;
    if (elapsed >= item.periodMs) return 0;
    const uint32_t remaining = item.periodMs - elapsed;
    if (remaining < waitMs) waitMs = remaining;
  }
  return waitMs;
}
