#include "hooks.h"
#include "registry.h"
#include "port_registry.h"                 // ★ 포트 레지스트리
#include "modules/express_emotion_tool.h"
#include "modules/impact_outport.h"        

void register_tools(ToolRegistry& reg, const ToolConfig& cfg) {

}



// ★ 여기서 포트 등록
void register_ports(PortRegistry& reg, const PortConfig& cfg) {
  (void)cfg;

  
}
