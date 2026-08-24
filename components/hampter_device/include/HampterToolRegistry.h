#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

#include "HampterTool.h"

class ToolRegistry {
 public:
  static constexpr size_t kCapacity = CONFIG_HAMPTER_MAX_TOOLS;

  void add(ITool* tool);
  bool initAll();

  size_t count() const { return count_; }
  ITool* at(size_t index) const;
  ITool* find(const char* name) const;
  bool valid() const { return error_ == nullptr; }
  const char* error() const { return error_; }

 private:
  ITool* tools_[kCapacity]{};
  size_t count_ = 0;
  const char* error_ = nullptr;
};
