#include "HampterDevice.h"

#include <esp_heap_caps.h>
#include <new>
#include <string.h>

#include "Runtime.h"

namespace {

HampterDevice* activeDevice = nullptr;

HampterDeviceConfig makeConfig(const char* name, const char* version) {
  HampterDeviceConfig config;
  config.name = name ? name : "hampter-device";
  config.version = version ? version : "0.1.0";
  return config;
}

template <size_t Size>
void ownText(const char* source, const char* fallback,
             char (&destination)[Size]) {
  if (source == nullptr) source = fallback;
  strncpy(destination, source, Size - 1);
  destination[Size - 1] = '\0';
}

template <size_t Size>
void ownBoundedMetadata(const char* source, const char* fallback,
                        char (&destination)[Size]) {
  if (source == nullptr) source = fallback;
  const size_t length = strlen(source);
  if (length == 0 || length >= Size) {
    destination[0] = '\0';
    return;
  }
  memcpy(destination, source, length + 1);
}

}  // namespace

HampterDevice::HampterDevice(const HampterDeviceConfig& config)
    : config_(config) {
  ownBoundedMetadata(config.name, "hampter-device", ownedName_);
  ownText(config.version, "0.1.0", ownedVersion_);
  ownBoundedMetadata(config.icon, "chip", ownedIcon_);
  config_.name = ownedName_;
  config_.version = ownedVersion_;
  config_.icon = ownedIcon_;
  activeDevice = this;
}

HampterDevice::HampterDevice(const char* name, const char* version)
    : HampterDevice(makeConfig(name, version)) {}

HampterDevice::~HampterDevice() {
  delete runtime_;
  if (activeDevice == this) activeDevice = nullptr;
}

void HampterDevice::registerHooks() {
  if (hooksRegistered_) return;
  const ToolConfig toolConfig;
  const PortConfig portConfig;
  register_tools(tools_, toolConfig);
  register_ports(ports_, portConfig);
  hooksRegistered_ = true;
}

void HampterDevice::begin() {
  if (runtime_ != nullptr) return;
  if (config_.serialBaud != 0) {
    Serial.begin(config_.serialBaud);
    // USB Serial/JTAG must never stall the app task when no terminal is
    // draining logs. HWCDC otherwise retries a full TX ring for roughly two
    // seconds per write, which also freezes the product's application loop.
    Serial.setTxTimeoutMs(0);
    const uint32_t deadline = millis() + 1500;
    while (!Serial && static_cast<int32_t>(deadline - millis()) > 0) delay(10);
  }

  if (!registriesInitialized_) {
    if (config_.autoRegisterHooks) registerHooks();
    if (!tools_.valid() || !ports_.valid() || !tools_.initAll()) {
      Serial.println("[HAMPTER] registry initialization failed");
      return;
    }
    registriesInitialized_ = true;
  }

  runtime_ =
      new (std::nothrow) hampter::internal::Runtime(config_, tools_, ports_);
  if (runtime_ == nullptr) {
    Serial.println("[HAMPTER] runtime allocation failed");
    return;
  }
  if (!runtime_->begin()) {
    Serial.printf("[HAMPTER] runtime initialization failed: %s\n",
                  runtime_->lastError());
    delete runtime_;
    runtime_ = nullptr;
    return;
  }

  Serial.printf(
      "[HAMPTER] %s %s tools=%u inports=%u outports=%u free_heap=%u\n",
      config_.name, config_.version, static_cast<unsigned>(tools_.count()),
      static_cast<unsigned>(ports_.inportCount()),
      static_cast<unsigned>(ports_.outportCount()),
      static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)));
}

void HampterDevice::loop() {
  if (runtime_ == nullptr) {
    delay(1);
    return;
  }

  runtime_->loop();
  const uint32_t now = millis();
  ports_.tickAll(now);

  uint32_t waitMs = ports_.millisecondsUntilNextTick(
      now, CONFIG_HAMPTER_APP_IDLE_MAX_MS);
  if (config_.resetButtonPin >= 0 && waitMs > 10) waitMs = 10;
  runtime_->waitForAppWork(waitMs);
}

void HampterDevice::poll() {
  if (runtime_ == nullptr) return;
  runtime_->loop();
  const uint32_t now = millis();
  ports_.tickAll(now);
}

void HampterDevice::waitForWork(uint32_t maximumMs) {
  if (maximumMs == 0) return;
  if (runtime_ == nullptr) {
    delay(1);
    return;
  }

  const uint32_t now = millis();
  uint32_t waitMs = ports_.millisecondsUntilNextTick(
      now, maximumMs);
  if (config_.resetButtonPin >= 0 && waitMs > 10) waitMs = 10;
  runtime_->waitForAppWork(waitMs);
}

HampterDeviceState HampterDevice::state() const {
  return runtime_ ? runtime_->state() : HampterDeviceState::Error;
}

bool HampterDevice::online() const {
  return runtime_ != nullptr && runtime_->online();
}

const char* HampterDevice::lastError() const {
  return runtime_ ? runtime_->lastError() : "runtime not initialized";
}

const char* HampterDevice::deviceId() const {
  if (runtime_ == nullptr ||
      !runtime_->copyDeviceId(deviceIdView_, sizeof(deviceIdView_))) {
    deviceIdView_[0] = '\0';
  }
  return deviceIdView_;
}

const char* HampterDevice::objectId() const {
  if (runtime_ == nullptr ||
      !runtime_->copyObjectId(objectIdView_, sizeof(objectIdView_))) {
    objectIdView_[0] = '\0';
  }
  return objectIdView_;
}

bool HampterDevice::publishPortData(const char* name, float value) {
  return runtime_ != nullptr && runtime_->publishPort(name, value);
}

bool port_publish_data(const char* name, float value) {
  return activeDevice != nullptr && activeDevice->publishPortData(name, value);
}

void port_set_outport_value(const char* name, float value) {
  (void)port_publish_data(name, value);
}

float port_get_inport_value(const char* name) {
  return activeDevice != nullptr
             ? activeDevice->ports().getInPortValue(name, NAN)
             : NAN;
}
