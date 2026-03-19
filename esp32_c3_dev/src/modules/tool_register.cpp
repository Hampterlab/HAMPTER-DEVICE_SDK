#include "hooks.h"
#include "registry.h"
#include "port_registry.h"
#include "express_emotion_tool.h"

static CreatePatternTool g_create_pattern_tool;
static ChangeSlotTool g_change_slot_tool;
static SlotStatusTool g_slot_status_tool;

void register_tools(ToolRegistry& reg, const ToolConfig& cfg) {
  (void)cfg;
  reg.add(&g_create_pattern_tool);
  reg.add(&g_change_slot_tool);
  reg.add(&g_slot_status_tool);
}

void register_ports(PortRegistry& reg, const PortConfig& cfg) {
  (void)cfg;
  reg.addInPort("var_a", "Primary modulation input for patterns", 0.0f, 1.0f, 0.0f);
  reg.addInPort("var_b", "Secondary modulation input for patterns", 0.0f, 1.0f, 0.0f);
  reg.addInPort("var_c", "Tertiary modulation input for patterns", 0.0f, 1.0f, 0.0f);
}
