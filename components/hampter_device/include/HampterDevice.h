#pragma once

#include <Arduino.h>

#include "HampterHooks.h"
#include "HampterPorts.h"
#include "HampterToolRegistry.h"

namespace hampter::internal {
class Runtime;
}

struct HampterDeviceConfig {
  const char* name = "hampter-device";
  const char* version = "0.1.0";
  const char* icon = "chip";
  uint32_t serialBaud = 115200;
  bool autoRegisterHooks = true;
  int resetButtonPin = -1;
  bool resetButtonActiveLow = true;
  uint32_t resetHoldMs = 5000;
};

enum class HampterDeviceState : uint8_t {
  Starting,
  Provisioning,
  Connecting,
  Online,
  Error,
};

struct HampterDeviceStatus {
  HampterDeviceState state = HampterDeviceState::Starting;

  constexpr bool isProvisioning() const {
    return state == HampterDeviceState::Provisioning;
  }
  constexpr bool isConnecting() const {
    return state == HampterDeviceState::Connecting;
  }
  constexpr bool isOnline() const {
    return state == HampterDeviceState::Online;
  }
};

class HampterDevice {
 public:
  explicit HampterDevice(
      const HampterDeviceConfig& config = HampterDeviceConfig{});
  HampterDevice(const char* deviceName, const char* firmwareVersion);
  ~HampterDevice();

  HampterDevice(const HampterDevice&) = delete;
  HampterDevice& operator=(const HampterDevice&) = delete;

  void begin();
  // Runs all application-side HAMPTER work that is ready now and returns
  // without an intentional wait. Network, provisioning, TLS, and ObjectLink
  // socket work remain on the resident I/O task.
  void poll();
  // Waits for Tool/InPort/reset-button work or a due OutPort, for no longer
  // than maximumMs. A zero maximum is strictly nonblocking.
  void waitForWork(uint32_t maximumMs);
  // Backward-compatible scheduler point using the configured app idle bound.
  void loop();

  ToolRegistry& tools() { return tools_; }
  PortRegistry& ports() { return ports_; }
  ToolRegistry& getToolRegistry() { return tools_; }
  PortRegistry& getPortRegistry() { return ports_; }

  void registerHooks();
  HampterDeviceState state() const;
  HampterDeviceStatus status() const { return HampterDeviceStatus{state()}; }
  bool online() const;
  const char* lastError() const;

  // Stable factory identity. It becomes non-empty after the background runtime
  // has loaded the device identity. The pointer is owned by this instance.
  const char* deviceId() const;
  // Hub-issued Object identity. This is empty until enrollment succeeds.
  const char* objectId() const;

  bool publishPortData(const char* portName, float value);

 private:
  HampterDeviceConfig config_;
  char ownedName_[65]{};
  char ownedVersion_[33]{};
  char ownedIcon_[97]{};
  mutable char deviceIdView_[97]{};
  mutable char objectIdView_[97]{};
  ToolRegistry tools_;
  PortRegistry ports_;
  hampter::internal::Runtime* runtime_ = nullptr;
  bool hooksRegistered_ = false;
  bool registriesInitialized_ = false;
};
