#pragma once
#include <Arduino.h>
inline String topicAnnounce(const String& id){ return String("mcp/dev/")+id+ "/announce"; }
inline String topicStatus  (const String& id){ return String("mcp/dev/")+id+ "/status"; }
inline String topicCmd     (const String& id){ return String("mcp/dev/")+id+ "/cmd"; }
inline String topicEvents  (const String& id){ return String("mcp/dev/")+id+ "/events"; }
inline String topicPortsAnnounce(const String& id){ return String("mcp/dev/")+id+ "/ports/announce"; }
inline String topicPortsData    (const String& id){ return String("mcp/dev/")+id+ "/ports/data"; }
inline String topicPortsSet     (const String& id){ return String("mcp/dev/")+id+ "/ports/set"; }
