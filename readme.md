# HAMPTER Device SDK
![HAMPTER](./hampter.png)

Firmware SDK for building HAMPTER-compatible peripheral devices on MCU.

---

## Quick Start

### Supported MCU
- ESP32-C3 (I only tested this one. But you can add more.)

### Prerequisites
- [PlatformIO](https://platformio.org/) (VS Code extension recommended)
- MCU

### Setup
1. Create a new PlatformIO project.
2. Copy the contents of the `SDK/` folder into your project.
3. **Write your custom tools in the `src/modules/` directory.**
4. Build and upload to your device.
5. Device will create a Wi-Fi hotspot for initial configuration.

---

## Connecting to Core Server

### Step 1: Start Core Server

On your PC/server:
```bash
cd core_server
docker-compose up -d
```

This starts:
- **MQTT Broker** on port `1883`
- **MCP Bridge** on port `8083`

### Step 2: Configure Device (First Time)

1. **Power on the device** - It enters provisioning mode
2. **Connect to device Wi-Fi** - Look for AP named `HAMPTER-XXXXXX`
3. **Open browser** - Go to `http://192.168.4.1`
4. **Enter configuration**:
   - **Wi-Fi SSID**: Your network name
   - **Wi-Fi Password**: Your network password
   - **MQTT Host**: IP address of Core Server (e.g., `192.168.1.100`)
   - **MQTT Port**: `1883`
   - **Device Name**: Friendly name for your device
5. **Save and restart**

### Step 3: Verify Connection

The device will:
1. Connect to your Wi-Fi
2. Connect to MQTT broker
3. Send **announce** message with available tools
4. Start publishing **status** every 30 seconds

**Check connection:**
```bash
# Subscribe to all device messages
mosquitto_sub -h localhost -t "mcp/dev/#" -v
```

You should see:
```
mcp/dev/dev-XXXXXX/announce {"name":"MyDevice","tools":[...]}
mcp/dev/dev-XXXXXX/status {"online":true}
```

### Step 4: Use from Claude Desktop

Once connected, restart Claude Desktop. Your device tools will appear in the MCP tool list.

---

## File Structure

```
SDK/
├── src/
│   ├── apps/
│   │   └── YOUR_PROJECT_NAME/
│   │       └── main.cpp           # Main entry point
│   ├── modules/
│   │   ├── your_tool.h            # Your custom tools (PUT YOUR CODE HERE)
│   │   └── tool_register.cpp      # Tool registration
│   └── transports/
│       └── topics.h               # MQTT topic definitions
├── lib/
│   ├── mcp-sdk/                   # Core MCP library
│   ├── port/                      # Port system library
│   └── provisioning/              # Wi-Fi provisioning
└── include/
    ├── config.hpp                 # Configuration
    └── tool.h                     # ITool interface
```

---

## Writing Custom Tools

### 1. Create Tool Class

Create a header file in `src/modules/`:

```cpp
// src/modules/my_tool.h
#pragma once
#include "tool.h"

class MyTool : public ITool {
public:
  bool init() override {
    // Initialize hardware (called once at startup)
    return true;
  }

  const char* name() const override { 
    return "my_tool_name";  // This name is shown to LLM
  }

  void describe(JsonObject& tool) override {
    tool["name"] = name();
    tool["description"] = "What this tool does (LLM reads this)";
    
    auto params = tool.createNestedObject("parameters");
    params["type"] = "object";
    
    auto props = params.createNestedObject("properties");
    props["message"]["type"] = "string";
    props["message"]["description"] = "Message to display";
    
    auto req = params.createNestedArray("required");
    req.add("message");
  }

  bool invoke(JsonObjectConst args, ObservationBuilder& out) override {
    // Called when LLM invokes this tool
    const char* msg = args["message"] | "default";
    
    // Your logic here...
    Serial.println(msg);
    
    out.success("Done!");  // Response to LLM
    return true;
  }
};
```

### 2. Register Tool

Edit `src/modules/tool_register.cpp`:

```cpp
#include "registry.h"
#include "my_tool.h"

// Instantiate your tools
static MyTool myTool;

void registerAllTools(ToolRegistry& reg) {
  reg.add(&myTool);
}
```

### 3. Build and Upload

```bash
pio run -t upload
```

Your tool will be announced to Core Server automatically.

---

## Port System (Experimental)

### OutPort: Send Data to Core Server

```cpp
#include "port_registry.h"

extern PortRegistry g_portRegistry;

// Register outport (in init or setup)
g_portRegistry.addOutPort("temperature", "float", "Current temperature");

// Publish value (anywhere in code)
g_portRegistry.publishOutPort("temperature", 25.5);
```

### InPort: Receive Data from Core Server

```cpp
// Register inport with callback
g_portRegistry.addInPort("brightness", "float", "LED brightness", 
  [](float value) {
    analogWrite(LED_PIN, (int)value);
  }
);
```

---

## MQTT Topics

| Topic | Direction | Description |
|-------|-----------|-------------|
| `mcp/dev/{id}/announce` | → Server | Register tools and ports |
| `mcp/dev/{id}/status` | → Server | Online/offline heartbeat |
| `mcp/dev/{id}/cmd` | ← Server | Tool invocation from LLM |
| `mcp/dev/{id}/events` | → Server | Tool execution result |
| `mcp/dev/{id}/ports/announce` | → Server | Register ports |
| `mcp/dev/{id}/ports/data` | → Server | OutPort values |
| `mcp/dev/{id}/ports/set` | ← Server | InPort values |

---

## Troubleshooting

### Device not appearing in Core Server
1. Check Wi-Fi connection (Serial monitor)
2. Verify MQTT host IP is correct
3. Check MQTT broker is running: `docker ps`
4. Subscribe to topics: `mosquitto_sub -t "mcp/#" -v`

### Provisioning mode not starting
- Hold BOOT button while powering on
- Or erase flash: `pio run -t erase`

### Tool not showing in Claude Desktop
1. Check device announce message contains your tool
2. Restart Claude Desktop after device connects
3. Check Core Server logs: `docker logs mcp-bridge`

### About TX Power

- TX Power is set to 8.5 dBm. It's because of BUG in ESP32-C3. If you use other MCU, you can change it.
