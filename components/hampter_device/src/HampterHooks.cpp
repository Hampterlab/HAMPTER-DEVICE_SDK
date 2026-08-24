#include "HampterHooks.h"

#include "HampterPorts.h"
#include "HampterToolRegistry.h"

void __attribute__((weak)) register_tools(ToolRegistry& registry,
                                          const ToolConfig& config) {
  (void)registry;
  (void)config;
}
void __attribute__((weak)) register_ports(PortRegistry& registry,
                                          const PortConfig& config) {
  (void)registry;
  (void)config;
}
