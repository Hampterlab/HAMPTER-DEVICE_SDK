#pragma once
#include <vector>
#include <ArduinoJson.h>
#include "tool.h"

class ToolRegistry{
 public:
  void add(ITool* t){ tools.push_back(t); }
  bool initAll(){ bool ok=true; for(auto* t:tools) ok=t->init() && ok; return ok; }
  String buildAnnounce(const String& device_id, const String& http_base);
  bool dispatch(const JsonDocument& cmd, String& outEventsJson, const String& http_base);
  const std::vector<ITool*>& list() const { return tools; }
 private:
  std::vector<ITool*> tools;
};