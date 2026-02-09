#pragma once
struct ToolConfig{ int dummy=0; };
class ToolRegistry;
void register_tools(ToolRegistry& reg, const ToolConfig& cfg) __attribute__((weak));