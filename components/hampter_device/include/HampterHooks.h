#pragma once

class PortRegistry;
class ToolRegistry;

// Compatibility tokens passed to the registration hooks. Runtime tuning does
// not belong here; ESP32-C3 bounds are compile-time Kconfig values.
struct ToolConfig {
  int dummy = 0;
};
struct PortConfig {
  int dummy = 0;
};

void register_tools(ToolRegistry& registry, const ToolConfig& config);
void register_ports(PortRegistry& registry, const PortConfig& config);
