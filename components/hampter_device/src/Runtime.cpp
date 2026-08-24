#include "Runtime.h"

#include <WiFi.h>
#include <errno.h>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_pm.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_vfs_eventfd.h>
#include <esp_wifi.h>
#include <driver/gpio.h>
#include <lwip/sockets.h>
#include <mbedtls/ecp.h>
#include <mbedtls/pk.h>
#include <sys/select.h>
#include <unistd.h>

#include <algorithm>
#include <float.h>
#include <math.h>
#include <string.h>
#include <utility>

#ifdef HAMPTER_ARDUINO_SSL_COMPAT
extern "C" void hampter_arduino_ssl_compat_anchor();
#endif

#if CONFIG_HAMPTER_PRODUCTION_BUILD && \
    CONFIG_HAMPTER_DEVELOPMENT_LOG_SOFTAP_PASSWORD
#error "Production HAMPTER firmware cannot log provisioning secrets"
#endif

namespace hampter::internal {
namespace {

constexpr char kTag[] = "hampter";
constexpr uint8_t kFramesPerStep = 4;
constexpr size_t kBytesPerStep = 8192;
constexpr uint32_t kReconnectMinMs = 500;
constexpr uint32_t kReconnectMaxMs = 30000;
constexpr uint32_t kBusyRetryMs = 100;
constexpr uint32_t kFallbackSocketPollMs = 20;
constexpr uint32_t kWifiJoinFallbackPollMs = 100;
constexpr uint32_t kIoSafetyWakeMs = 5000;
// Traffic-tested on the target ESP32-C3 board. Raw 34 is an exact ESP-IDF
// power step (8.5 dBm) and avoids the reproducible raw-20/5 dBm loss notch.
constexpr int8_t kWifiTxPowerQuarterDbm = 34;
const char* kAlpn[] = {"hampter-objectlink/2", nullptr};

bool timeReached(uint32_t now, uint32_t deadline) {
  return static_cast<int32_t>(now - deadline) >= 0;
}

uint32_t millisecondsUntil(uint32_t now, uint32_t deadline) {
  return timeReached(now, deadline) ? 0 : deadline - now;
}

void reduceWait(uint32_t candidate, uint32_t& waitMs) {
  if (candidate < waitMs) waitMs = candidate;
}

template <size_t Size>
void copyText(const char* source, char (&destination)[Size]) {
  if (source == nullptr) source = "";
  strncpy(destination, source, Size - 1);
  destination[Size - 1] = '\0';
}

void wipeStringSecret(String& value) {
  for (size_t i = 0; i < value.length(); ++i) value.setCharAt(i, '\0');
  value = "";
}

void wipeBytes(void* value, size_t length) {
  volatile uint8_t* bytes = static_cast<volatile uint8_t*>(value);
  while (bytes != nullptr && length-- > 0) *bytes++ = 0;
}

void wipeProvisioningSecrets(ProvisioningRequest& request) {
  wipeStringSecret(request.wifiPassword);
  wipeStringSecret(request.hub.enrollmentToken);
}

void clearProvisioningRequest(ProvisioningRequest& request) {
  wipeProvisioningSecrets(request);
  request = ProvisioningRequest{};
}

void clearHubBootstrap(HubBootstrap& bootstrap) {
  wipeStringSecret(bootstrap.enrollmentToken);
  bootstrap = HubBootstrap{};
}

void wipeStoredCredentialSecrets(StoredCredentials& credentials) {
  wipeStringSecret(credentials.wifiPassword);
  wipeStringSecret(credentials.deviceCredential);
}

void clearStoredCredentials(StoredCredentials& credentials) {
  wipeStoredCredentialSecrets(credentials);
  credentials = StoredCredentials{};
}

class StoredCredentialSecretGuard {
 public:
  explicit StoredCredentialSecretGuard(StoredCredentials& credentials)
      : credentials_(credentials) {}
  ~StoredCredentialSecretGuard() {
    wipeStoredCredentialSecrets(credentials_);
  }

 private:
  StoredCredentials& credentials_;
};

class ByteWiper {
 public:
  ByteWiper(void* value, size_t length)
      : value_(static_cast<volatile uint8_t*>(value)), length_(length) {}
  ~ByteWiper() {
    wipeBytes(const_cast<uint8_t*>(value_), length_);
  }

 private:
  volatile uint8_t* value_;
  size_t length_;
};

class WipingHeapAllocator final : public Allocator {
 public:
  void* allocate(size_t size) override {
    if (size == 0 || size > SIZE_MAX - sizeof(BlockHeader)) return nullptr;
    auto* block = static_cast<BlockHeader*>(malloc(sizeof(BlockHeader) + size));
    if (block == nullptr) return nullptr;
    block->size = size;
    return block + 1;
  }

  void deallocate(void* pointer) override {
    if (pointer == nullptr) return;
    auto* block = static_cast<BlockHeader*>(pointer) - 1;
    const size_t size = block->size;
    wipeBytes(block, sizeof(BlockHeader) + size);
    free(block);
  }

  void* reallocate(void* pointer, size_t newSize) override {
    if (pointer == nullptr) return allocate(newSize);
    if (newSize == 0) {
      deallocate(pointer);
      return nullptr;
    }
    auto* oldBlock = static_cast<BlockHeader*>(pointer) - 1;
    const size_t oldSize = oldBlock->size;
    void* replacement = allocate(newSize);
    if (replacement == nullptr) return nullptr;
    memcpy(replacement, pointer, std::min(oldSize, newSize));
    deallocate(pointer);
    return replacement;
  }

 private:
  struct alignas(max_align_t) BlockHeader {
    size_t size;
  };
};

bool validIdentifier(const char* value, size_t maximum = 96) {
  if (value == nullptr) return false;
  const size_t length = strlen(value);
  if (length == 0 || length > maximum) return false;
  for (size_t i = 0; i < length; ++i) {
    const char c = value[i];
    if (!(isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' ||
          c == ':' || c == '/' || c == '-')) {
      return false;
    }
  }
  return true;
}

bool validIcon(const char* value) {
  if (value == nullptr) return false;
  const size_t length = strlen(value);
  if (length == 0 || length > 96) return false;
  for (size_t i = 0; i < length; ++i) {
    const char c = value[i];
    if (!(isalnum(static_cast<unsigned char>(c)) || c == '_' || c == ':' ||
          c == '/' || c == '-')) {
      return false;
    }
  }
  return true;
}

bool decodeUtf8Codepoint(const uint8_t* value, size_t length, size_t& offset,
                         uint32_t& codepoint) {
  if (offset >= length) return false;
  const uint8_t first = value[offset++];
  if (first < 0x80) {
    codepoint = first;
    return true;
  }

  size_t continuation = 0;
  if (first >= 0xC2 && first <= 0xDF) {
    codepoint = first & 0x1F;
    continuation = 1;
  } else if (first >= 0xE0 && first <= 0xEF) {
    codepoint = first & 0x0F;
    continuation = 2;
  } else if (first >= 0xF0 && first <= 0xF4) {
    codepoint = first & 0x07;
    continuation = 3;
  } else {
    return false;
  }
  if (continuation > length - offset) return false;
  for (size_t i = 0; i < continuation; ++i) {
    const uint8_t next = value[offset++];
    if ((next & 0xC0) != 0x80) return false;
    codepoint = (codepoint << 6) | (next & 0x3F);
  }
  return !((continuation == 2 && codepoint < 0x800) ||
           (continuation == 3 && codepoint < 0x10000) ||
           (codepoint >= 0xD800 && codepoint <= 0xDFFF) ||
           codepoint > 0x10FFFF);
}

bool isUnicodeWhitespace(uint32_t codepoint) {
  return (codepoint >= 0x09 && codepoint <= 0x0D) || codepoint == 0x20 ||
         codepoint == 0x85 || codepoint == 0xA0 || codepoint == 0x1680 ||
         (codepoint >= 0x2000 && codepoint <= 0x200A) ||
         codepoint == 0x2028 || codepoint == 0x2029 || codepoint == 0x202F ||
         codepoint == 0x205F || codepoint == 0x3000;
}

bool isUnicodeControl(uint32_t codepoint) {
  return codepoint <= 0x1F || (codepoint >= 0x7F && codepoint <= 0x9F);
}

bool validDisplayName(const char* value) {
  if (value == nullptr) return false;
  const size_t length = strlen(value);
  if (length == 0 || length > 64) return false;
  const auto* bytes = reinterpret_cast<const uint8_t*>(value);
  size_t offset = 0;
  uint32_t firstCodepoint = 0;
  uint32_t lastCodepoint = 0;
  bool first = true;
  while (offset < length) {
    uint32_t codepoint = 0;
    if (!decodeUtf8Codepoint(bytes, length, offset, codepoint) ||
        isUnicodeControl(codepoint)) {
      return false;
    }
    if (first) {
      firstCodepoint = codepoint;
      first = false;
    }
    lastCodepoint = codepoint;
  }
  return !isUnicodeWhitespace(firstCodepoint) &&
         !isUnicodeWhitespace(lastCodepoint);
}

size_t alignUp(size_t value, size_t alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

// Stops descending as soon as the configured depth is exceeded. Unlike
// JsonVariantConst::nesting(), recursion is therefore capped even if Tool code
// attempted to construct an excessively deep value.
bool resultNestingWithinLimit(JsonVariantConst value, size_t remaining) {
  if (value.is<JsonArrayConst>()) {
    if (remaining == 0) return false;
    for (JsonVariantConst child : value.as<JsonArrayConst>()) {
      if (!resultNestingWithinLimit(child, remaining - 1)) return false;
    }
  } else if (value.is<JsonObjectConst>()) {
    if (remaining == 0) return false;
    for (JsonPairConst pair : value.as<JsonObjectConst>()) {
      if (!resultNestingWithinLimit(pair.value(), remaining - 1)) return false;
    }
  }
  return true;
}

uint64_t monotonicMs() {
  return static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL;
}

uint32_t floatBits(float value) {
  uint32_t bits = 0;
  memcpy(&bits, &value, sizeof(bits));
  return bits;
}

float bitsFloat(uint32_t bits) {
  float value = 0;
  memcpy(&value, &bits, sizeof(value));
  return value;
}

char hexDigit(uint8_t value) {
  return value < 10 ? static_cast<char>('0' + value)
                    : static_cast<char>('a' + value - 10);
}

String hexEncode(const uint8_t* bytes, size_t length) {
  String output;
  if (bytes == nullptr || !output.reserve(length * 2)) return String();
  for (size_t i = 0; i < length; ++i) {
    output += hexDigit(bytes[i] >> 4);
    output += hexDigit(bytes[i] & 0x0F);
  }
  return output;
}

int hexNibble(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
}

bool hexDecode(const String& encoded, uint8_t* output, size_t capacity,
               size_t& length) {
  length = 0;
  int high = -1;
  for (size_t i = 0; i < encoded.length(); ++i) {
    if (encoded[i] == ':' || encoded[i] == ' ') continue;
    const int nibble = hexNibble(encoded[i]);
    if (nibble < 0) return false;
    if (high < 0) {
      high = nibble;
    } else {
      if (length >= capacity) return false;
      output[length++] = static_cast<uint8_t>((high << 4) | nibble);
      high = -1;
    }
  }
  return high < 0;
}

bool fingerprintMatches(const String& encoded, const uint8_t* bytes,
                        size_t length) {
  uint8_t expected[32];
  size_t expectedLength = 0;
  if (length != sizeof(expected) ||
      !hexDecode(encoded, expected, sizeof(expected), expectedLength) ||
      expectedLength != sizeof(expected)) {
    return false;
  }
  uint8_t difference = 0;
  for (size_t i = 0; i < length; ++i) difference |= expected[i] ^ bytes[i];
  return difference == 0;
}

void randomBytes(uint8_t* output, size_t length) {
  esp_fill_random(output, length);
}

int mbedRandom(void*, unsigned char* output, size_t length) {
  randomBytes(output, length);
  return 0;
}

bool generateP256PublicKey(uint8_t* output, size_t capacity, size_t& length) {
  mbedtls_pk_context key;
  mbedtls_pk_init(&key);
  const mbedtls_pk_info_t* info = mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY);
  if (info == nullptr || mbedtls_pk_setup(&key, info) != 0) {
    mbedtls_pk_free(&key);
    return false;
  }
  mbedtls_ecp_keypair* ec = mbedtls_pk_ec(key);
  if (ec == nullptr ||
      mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, ec, mbedRandom, nullptr) !=
          0) {
    mbedtls_pk_free(&key);
    return false;
  }
  const int written = mbedtls_pk_write_pubkey_der(&key, output, capacity);
  if (written <= 0 || static_cast<size_t>(written) > capacity) {
    mbedtls_pk_free(&key);
    return false;
  }
  memmove(output, output + capacity - written, written);
  length = static_cast<size_t>(written);
  mbedtls_pk_free(&key);
  return true;
}

}  // namespace

Runtime::Runtime(const HampterDeviceConfig& config, ToolRegistry& tools,
                 PortRegistry& ports)
    : config_(config),
      tools_(tools),
      ports_(ports),
      appResultDocument_(&appResultArena_) {}

void* Runtime::ToolResultArena::allocate(size_t size) {
  if (size == 0) return nullptr;
  constexpr size_t alignment = alignof(max_align_t);
  const size_t dataOffset = alignUp(used_ + sizeof(BlockHeader), alignment);
  if (dataOffset > sizeof(storage_) || size > sizeof(storage_) - dataOffset) {
    return nullptr;
  }
  auto* header = reinterpret_cast<BlockHeader*>(
      storage_ + dataOffset - sizeof(BlockHeader));
  header->size = size;
  used_ = dataOffset + size;
  return storage_ + dataOffset;
}

void Runtime::ToolResultArena::deallocate(void*) {
  // Individual frees are deliberately deferred. JsonDocument::clear() first
  // drops all references, then resetAppResultDocument() reclaims the arena in
  // one constant-time operation without heap fragmentation.
}

void* Runtime::ToolResultArena::reallocate(void* pointer, size_t newSize) {
  if (pointer == nullptr) return allocate(newSize);
  if (newSize == 0) return nullptr;

  auto* bytes = static_cast<uint8_t*>(pointer);
  if (bytes < storage_ + sizeof(BlockHeader) ||
      bytes >= storage_ + sizeof(storage_)) {
    return nullptr;
  }
  auto* header = reinterpret_cast<BlockHeader*>(bytes - sizeof(BlockHeader));
  const size_t oldSize = header->size;
  const size_t offset = static_cast<size_t>(bytes - storage_);
  if (offset > used_ || oldSize > used_ - offset) return nullptr;

  if (offset + oldSize == used_) {
    if (newSize > sizeof(storage_) - offset) return nullptr;
    header->size = newSize;
    used_ = offset + newSize;
    return pointer;
  }
  if (newSize <= oldSize) {
    header->size = newSize;
    return pointer;
  }

  void* replacement = allocate(newSize);
  if (replacement == nullptr) return nullptr;
  memcpy(replacement, pointer, oldSize);
  return replacement;
}

Runtime::~Runtime() {
  stopping_.store(true, std::memory_order_release);
  wakeIo();
  wakeApp();
  if (ioTaskCreated_.load(std::memory_order_acquire) && ioTaskDone_ != nullptr) {
    (void)xSemaphoreTake(ioTaskDone_, portMAX_DELAY);
    if (TaskHandle_t task = ioTaskHandle_.load(std::memory_order_acquire);
        task != nullptr) {
      vTaskDelete(task);
      ioTaskHandle_.store(nullptr, std::memory_order_release);
    }
  }
  removeNetworkEventHandlers();
  const int wakeFd = wakeFd_.exchange(-1, std::memory_order_acq_rel);
  if (wakeFd >= 0) ::close(wakeFd);
  portal_.stop();
  releaseProvisioningPowerLock();
  clearStoredCredentials(credentials_);
  clearProvisioningRequest(pendingProvisioning_);
  clearHubBootstrap(bootstrap_);
  wipeStringSecret(identity_.softApPassword);
}

bool Runtime::begin() {
  // Pull the hash-pinned Arduino TLS compatibility TU out of this component's
  // static archive before NetworkClientSecure introduces its references.
#ifdef HAMPTER_ARDUINO_SSL_COMPAT
  hampter_arduino_ssl_compat_anchor();
#endif
  if (!validDisplayName(config_.name) || !validIcon(config_.icon) ||
      ports_.totalCount() > CONFIG_HAMPTER_MAX_PORTS) {
    setError("invalid Object name, icon, or Port registry", true);
    return false;
  }
  for (size_t i = 0; i < tools_.count(); ++i) {
    ITool* tool = tools_.at(i);
    if (tool == nullptr || !validIdentifier(tool->name())) {
      setError("invalid Tool name", true);
      return false;
    }
    for (size_t p = 0; p < ports_.totalCount(); ++p) {
      const PortRegistry::Entry* port = ports_.entry(p);
      if (port != nullptr && strcmp(tool->name(), port->name) == 0) {
        setError("Tool and Port names share one ObjectLink namespace", true);
        return false;
      }
    }
  }
  for (size_t i = 0; i < ports_.totalCount(); ++i) {
    const PortRegistry::Entry* port = ports_.entry(i);
    if (port == nullptr || !validIdentifier(port->name) ||
        port->description == nullptr || port->description[0] == '\0') {
      setError("invalid Port metadata", true);
      return false;
    }
    portSlots_[i].outbound =
        port->direction == PortRegistry::Direction::Out;
  }
  if (!buildManifest()) {
    setError("Tool/Port manifest exceeds the bounded ObjectLink frame", true);
    return false;
  }
  if (config_.resetButtonPin >= 0) {
    if (!GPIO_IS_VALID_GPIO(config_.resetButtonPin)) {
      setError("reset button GPIO is invalid for this ESP32-C3", true);
      return false;
    }
    gpio_config_t button{};
    button.pin_bit_mask = 1ULL << config_.resetButtonPin;
    button.mode = GPIO_MODE_INPUT;
    button.pull_up_en = config_.resetButtonActiveLow ? GPIO_PULLUP_ENABLE
                                                     : GPIO_PULLUP_DISABLE;
    button.pull_down_en = config_.resetButtonActiveLow
                              ? GPIO_PULLDOWN_DISABLE
                              : GPIO_PULLDOWN_ENABLE;
    button.intr_type = GPIO_INTR_DISABLE;
    if (gpio_config(&button) != ESP_OK) {
      setError("could not configure reset button GPIO", true);
      return false;
    }
  }
  appWake_ = xSemaphoreCreateBinaryStatic(&appWakeControl_);
  if (appWake_ == nullptr) {
    setError("could not create HAMPTER application wake signal", true);
    return false;
  }
  ioTaskDone_ = xSemaphoreCreateBinaryStatic(&ioTaskDoneControl_);
  if (ioTaskDone_ == nullptr) {
    setError("could not create HAMPTER I/O completion signal", true);
    return false;
  }
  TaskHandle_t created = xTaskCreateStatic(
      ioTaskEntry, "hampter_io", sizeof(ioTaskStack_), this,
      CONFIG_HAMPTER_IO_TASK_PRIORITY,
      reinterpret_cast<StackType_t*>(ioTaskStack_), &ioTaskControl_);
  if (created == nullptr) {
    setError("could not create HAMPTER I/O task", true);
    return false;
  }
  ioTaskCreated_.store(true, std::memory_order_release);
  return true;
}

bool Runtime::buildManifest() {
  manifestDocument_.clear();
  JsonArray tools = manifestDocument_["tools"].to<JsonArray>();
  JsonDocument description;
  for (size_t i = 0; i < tools_.count(); ++i) {
    ITool* implementation = tools_.at(i);
    if (implementation == nullptr) return false;
    description.clear();
    JsonObject described = description.to<JsonObject>();
    implementation->describe(described);

    JsonObject tool = tools.add<JsonObject>();
    tool["name"] = implementation->name();
    const char* text = described["description"] | "";
    if (text[0] != '\0') tool["description"] = text;
    if (described["parameters"].is<JsonObject>()) {
      tool["input_schema"].set(
          described["parameters"].as<JsonVariantConst>());
    } else if (described["input_schema"].is<JsonObject>()) {
      tool["input_schema"].set(
          described["input_schema"].as<JsonVariantConst>());
    } else {
      tool["input_schema"].to<JsonObject>();
    }
    if (described["output_schema"].is<JsonObject>()) {
      tool["output_schema"].set(
          described["output_schema"].as<JsonVariantConst>());
    }
    JsonObject qos = tool["qos"].to<JsonObject>();
    qos["timeout_ms"] = std::clamp<uint32_t>(
        described["timeout_ms"] | 30000U, 1, 86400000);
    qos["idempotent"] = described["idempotent"] | false;
  }

  JsonArray ports = manifestDocument_["ports"].to<JsonArray>();
  for (size_t i = 0; i < ports_.totalCount(); ++i) {
    const PortRegistry::Entry* entry = ports_.entry(i);
    if (entry == nullptr) return false;
    JsonObject port = ports.add<JsonObject>();
    port["name"] = entry->name;
    const char* descriptionText = entry->description;
    if (entry->customOut != nullptr) {
      description.clear();
      JsonObject described = description.to<JsonObject>();
      entry->customOut->describe(described);
      const char* customDescription = described["description"] | "";
      if (customDescription[0] != '\0') descriptionText = customDescription;
    }
    String enriched(descriptionText);
    if (entry->unit != nullptr && entry->unit[0] != '\0') {
      enriched += " [unit: ";
      enriched += entry->unit;
      enriched += "]";
    }
    port["description"] = enriched;
    port["direction"] =
        entry->direction == PortRegistry::Direction::In ? "in" : "out";
    JsonObject schema = port["schema"].to<JsonObject>();
    schema["shape"] = "scalar";
    schema["max_items"] = 0;
  }

  const size_t encoded = measureMsgPack(manifestDocument_);
  if (manifestDocument_.overflowed() || encoded == 0 ||
      encoded + 512 > CONFIG_HAMPTER_FRAME_PAYLOAD_BYTES) {
    manifestDocument_.clear();
    return false;
  }
  manifestDocument_.shrinkToFit();
  return true;
}

void Runtime::loop() {
  appHandleResetButton();
  appHandlePorts();
  appHandleTools();
}

bool Runtime::appWorkPending() const {
  if (!toolJobs_.empty() && !toolResults_.full() &&
      toolResultBytes_.canPush(kToolWireMax)) {
    return true;
  }
  for (size_t i = 0; i < ports_.totalCount(); ++i) {
    if (!portSlots_[i].outbound &&
        portSlots_[i].inboundPending.load(std::memory_order_acquire)) {
      return true;
    }
  }
  return false;
}

void Runtime::wakeApp() {
  if (appWake_ != nullptr) (void)xSemaphoreGive(appWake_);
}

void Runtime::waitForAppWork(uint32_t maximumMs) {
  if (maximumMs == 0 || appWorkPending() || appWake_ == nullptr) {
    taskYIELD();
    return;
  }
  TickType_t ticks = pdMS_TO_TICKS(maximumMs);
  if (ticks == 0) ticks = 1;
  (void)xSemaphoreTake(appWake_, ticks);
}

const char* Runtime::lastError() const {
  portENTER_CRITICAL(&errorMux_);
  copyText(lastError_, lastErrorSnapshot_);
  portEXIT_CRITICAL(&errorMux_);
  return lastErrorSnapshot_;
}

bool Runtime::copyDeviceId(char* output, size_t capacity) const {
  if (output == nullptr || capacity == 0) return false;
  portENTER_CRITICAL(&identityMux_);
  const size_t length = strnlen(deviceIdSnapshot_, sizeof(deviceIdSnapshot_));
  if (length == 0 || length >= capacity) {
    output[0] = '\0';
    portEXIT_CRITICAL(&identityMux_);
    return false;
  }
  memcpy(output, deviceIdSnapshot_, length + 1);
  portEXIT_CRITICAL(&identityMux_);
  return true;
}

bool Runtime::copyObjectId(char* output, size_t capacity) const {
  if (output == nullptr || capacity == 0) return false;
  portENTER_CRITICAL(&identityMux_);
  const size_t length = strnlen(objectIdSnapshot_, sizeof(objectIdSnapshot_));
  if (length == 0 || length >= capacity) {
    output[0] = '\0';
    portEXIT_CRITICAL(&identityMux_);
    return false;
  }
  memcpy(output, objectIdSnapshot_, length + 1);
  portEXIT_CRITICAL(&identityMux_);
  return true;
}

void Runtime::publishIdentitySnapshots() {
  portENTER_CRITICAL(&identityMux_);
  copyText(identity_.deviceId.c_str(), deviceIdSnapshot_);
  copyText(credentials_.objectId.c_str(), objectIdSnapshot_);
  portEXIT_CRITICAL(&identityMux_);
}

bool Runtime::publishPort(const char* name, float value) {
  if (name == nullptr || name[0] == '\0' || !isfinite(value)) return false;
  const int index = ports_.findEntry(name, PortRegistry::Direction::Out);
  if (index < 0) return false;
  PortSlot& slot = portSlots_[index];
  slot.outboundBits.store(floatBits(value), std::memory_order_relaxed);
  uint32_t revision =
      slot.outboundRevision.fetch_add(1, std::memory_order_release) + 1;
  if (revision == 0) {
    slot.outboundRevision.fetch_add(1, std::memory_order_release);
  }
  wakeIo();
  return true;
}

void Runtime::appHandlePorts() {
  for (size_t i = 0; i < ports_.totalCount(); ++i) {
    PortSlot& slot = portSlots_[i];
    if (slot.outbound) continue;
    bool pending = false;
    uint32_t valueBits = 0;
    portENTER_CRITICAL(&portMux_);
    if (slot.inboundPending.load(std::memory_order_relaxed)) {
      valueBits = slot.inboundBits.load(std::memory_order_relaxed);
      slot.inboundPending.store(false, std::memory_order_relaxed);
      pending = true;
    }
    portEXIT_CRITICAL(&portMux_);
    if (pending) (void)ports_.applyInbound(i, bitsFloat(valueBits));
  }
}

void Runtime::resetAppResultDocument(uint64_t callId) {
  // clear() lets ArduinoJson release its internal references while the arena
  // is intact. reset() can then reclaim every block at once.
  appResultDocument_.clear();
  appResultArena_.reset();
  appResultDocument_["call_id"] = callId;
}

void Runtime::appHandleTools() {
  if (toolResults_.full() || !toolResultBytes_.canPush(kToolWireMax)) return;
  ToolJob job;
  if (!toolJobs_.tryPop(job)) return;
  size_t argumentLength = 0;
  if (!toolArguments_.tryPop(appArgumentScratch_,
                             sizeof(appArgumentScratch_), argumentLength) ||
      argumentLength != job.argumentLength) {
    setError("Tool argument byte-pool order violation", true);
    return;
  }
  if (job.epoch != sessionEpoch_.load(std::memory_order_acquire)) return;

  appArgumentsDocument_.clear();
  bool argumentsValid =
      !deserializeMsgPack(appArgumentsDocument_, appArgumentScratch_,
                          argumentLength,
                          DeserializationOption::NestingLimit(8)) &&
      appArgumentsDocument_.is<JsonObject>();
  ITool* tool = tools_.at(job.toolIndex);
  resetAppResultDocument(job.callId);
  JsonVariant resultSlot = appResultDocument_["value"].to<JsonVariant>();
  observation_.reset(resultSlot);
  bool invoked = false;
  if (argumentsValid && tool != nullptr &&
      (job.localDeadlineMs == 0 ||
       !timeReached(millis(), job.localDeadlineMs))) {
    invoked = tool->invoke(appArgumentsDocument_.as<JsonObjectConst>(),
                           observation_);
  }

  const bool success = invoked && observation_.ok_;
  if (success) {
    JsonVariantConst result = appResultDocument_["value"];
    const bool documentOverflowed = appResultDocument_.overflowed();
    const bool nestingValid =
        !documentOverflowed &&
        resultNestingWithinLimit(result, kToolResultNestingLimit);
    // measureMsgPack() is recursive; call it only after the bounded-depth
    // preflight above.
    const size_t resultLength = nestingValid ? measureMsgPack(result) : 0;
    const bool resultFits =
        !observation_.resultOverflowed_ && !documentOverflowed &&
        nestingValid && resultLength > 0 &&
        resultLength <= CONFIG_HAMPTER_TOOL_RESULT_MAX_BYTES;
    if (resultFits) {
      appResultDocument_["outcome"] = "success";
    } else {
      const bool tooDeep = !documentOverflowed && !nestingValid;
      // Reclaim the entire result arena before constructing the small failure
      // envelope, including when a handler consumed all available slots.
      resetAppResultDocument(job.callId);
      appResultDocument_["outcome"] = "failed";
      JsonObject error = appResultDocument_["error"].to<JsonObject>();
      error["code"] = documentOverflowed
                          ? "result_encoding_failed"
                          : (tooDeep ? "result_too_deep" : "result_too_large");
      error["message"] =
          documentOverflowed
              ? "Device could not allocate the Tool result"
              : (tooDeep
                     ? "Tool result exceeded the nesting limit"
                     : "Tool result exceeded the configured encoded size limit");
      error["retryable"] = false;
    }
  } else {
    // A failing handler may already have built a partial value. Reclaim it in
    // one operation before encoding the bounded error fields below.
    resetAppResultDocument(job.callId);
    const bool expired = job.localDeadlineMs != 0 &&
                         timeReached(millis(), job.localDeadlineMs);
    appResultDocument_["outcome"] = expired ? "timed_out" : "failed";
    JsonObject error = appResultDocument_["error"].to<JsonObject>();
    const char* rawCode = expired
                              ? "deadline_exceeded"
                              : (observation_.errorCode_[0] != '\0'
                                     ? observation_.errorCode_
                                     : "tool_failed");
    error["code"] = validIdentifier(rawCode) ? rawCode : "tool_failed";
    const char* rawMessage =
        expired
            ? "Tool deadline expired before execution"
            : (observation_.errorMessage_[0] != '\0'
                   ? observation_.errorMessage_
                    : (argumentsValid ? "Tool handler returned failure"
                                      : "Tool arguments were invalid"));
    if (rawMessage[0] == '\0') {
      rawMessage = argumentsValid ? "Tool handler returned failure"
                                  : "Tool arguments were invalid";
    }
    char safeMessage[161];
    copyText(rawMessage, safeMessage);
    error["message"] = safeMessage;
    error["retryable"] = false;
  }

  size_t payloadLength = measureMsgPack(appResultDocument_);
  if (appResultDocument_.overflowed() || payloadLength == 0 ||
      payloadLength > sizeof(appResultScratch_) ||
      serializeMsgPack(appResultDocument_, appResultScratch_,
                       sizeof(appResultScratch_)) != payloadLength) {
    resetAppResultDocument(job.callId);
    appResultDocument_["outcome"] = "failed";
    JsonObject error = appResultDocument_["error"].to<JsonObject>();
    error["code"] = "result_encoding_failed";
    error["message"] = "Device could not encode the Tool result";
    error["retryable"] = false;
    payloadLength = measureMsgPack(appResultDocument_);
    if (appResultDocument_.overflowed() || payloadLength == 0 ||
        payloadLength > sizeof(appResultScratch_) ||
        serializeMsgPack(appResultDocument_, appResultScratch_,
                         sizeof(appResultScratch_)) != payloadLength) {
      setError("could not encode bounded Tool failure", true);
      return;
    }
  }

  ToolResultRecord result;
  result.callId = job.callId;
  result.streamId = job.streamId;
  result.epoch = job.epoch;
  result.payloadLength = static_cast<uint16_t>(payloadLength);
  if (!toolResultBytes_.tryPush(appResultScratch_, payloadLength) ||
      !toolResults_.tryPush(result)) {
    setError("Tool result byte pool unexpectedly saturated", true);
    return;
  }
  wakeIo();
}

void Runtime::appHandleResetButton() {
  if (config_.resetButtonPin < 0 || resetHandled_) return;
  const bool raw =
      gpio_get_level(static_cast<gpio_num_t>(config_.resetButtonPin)) != 0;
  const bool pressed = config_.resetButtonActiveLow ? !raw : raw;
  if (!pressed) {
    resetPressedAt_ = 0;
    return;
  }
  if (resetPressedAt_ == 0) resetPressedAt_ = millis();
  if (millis() - resetPressedAt_ >= config_.resetHoldMs) {
    resetHandled_ = true;
    resetRequested_.store(true, std::memory_order_release);
    wakeIo();
  }
}

void Runtime::ioTaskEntry(void* context) {
  Runtime* runtime = static_cast<Runtime*>(context);
  runtime->ioTaskHandle_.store(xTaskGetCurrentTaskHandle(),
                               std::memory_order_release);
  runtime->ioTask();
}

void Runtime::networkEventHandler(void* context, esp_event_base_t,
                                  int32_t, void*) {
  Runtime* runtime = static_cast<Runtime*>(context);
  if (runtime != nullptr) runtime->wakeIo();
}

bool Runtime::configurePowerManagement() {
#if CONFIG_HAMPTER_LOW_POWER_PROFILE && CONFIG_PM_ENABLE && \
    CONFIG_FREERTOS_USE_TICKLESS_IDLE
  esp_pm_config_t requested{};
  requested.max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
  requested.min_freq_mhz = CONFIG_XTAL_FREQ;
  requested.light_sleep_enable = true;
  const esp_err_t configureResult = esp_pm_configure(&requested);
  esp_pm_config_t actual{};
  const esp_err_t readResult = esp_pm_get_configuration(&actual);
  const bool okay =
      configureResult == ESP_OK && readResult == ESP_OK &&
      actual.max_freq_mhz == requested.max_freq_mhz &&
      actual.min_freq_mhz == requested.min_freq_mhz &&
      actual.light_sleep_enable;
  if (!okay) {
    ESP_LOGE(kTag,
             "power profile failed configure=%s read=%s max=%d min=%d "
             "light_sleep=%d",
             esp_err_to_name(configureResult), esp_err_to_name(readResult),
             actual.max_freq_mhz, actual.min_freq_mhz,
             actual.light_sleep_enable);
    return false;
  }
  ESP_LOGI(kTag, "power profile DFS=%d..%dMHz auto_light_sleep=on",
           actual.min_freq_mhz, actual.max_freq_mhz);
  return true;
#else
  ESP_LOGW(kTag, "ESP32-C3 connected low-power profile disabled");
  return true;
#endif
}

bool Runtime::acquireProvisioningPowerLock() {
#if CONFIG_HAMPTER_LOW_POWER_PROFILE && CONFIG_PM_ENABLE
  if (provisioningPowerLockHeld_) return true;
  if (provisioningPowerLock_ == nullptr &&
      esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "hampter_setup",
                         &provisioningPowerLock_) != ESP_OK) {
    provisioningPowerLock_ = nullptr;
    return false;
  }
  if (esp_pm_lock_acquire(provisioningPowerLock_) != ESP_OK) return false;
  provisioningPowerLockHeld_ = true;
  return true;
#else
  return true;
#endif
}

void Runtime::releaseProvisioningPowerLock() {
#if CONFIG_HAMPTER_LOW_POWER_PROFILE && CONFIG_PM_ENABLE
  if (provisioningPowerLockHeld_ && provisioningPowerLock_ != nullptr) {
    (void)esp_pm_lock_release(provisioningPowerLock_);
    provisioningPowerLockHeld_ = false;
  }
  if (provisioningPowerLock_ != nullptr) {
    (void)esp_pm_lock_delete(provisioningPowerLock_);
    provisioningPowerLock_ = nullptr;
  }
#endif
}

bool Runtime::initializeWakeFd() {
  esp_vfs_eventfd_config_t config{};
  config.max_fds = 1;
  const esp_err_t registration = esp_vfs_eventfd_register(&config);
  if (registration != ESP_OK && registration != ESP_ERR_INVALID_STATE) {
    ESP_LOGW(kTag, "event wake VFS unavailable: %s",
             esp_err_to_name(registration));
    return false;
  }
  const int descriptor = eventfd(0, 0);
  if (descriptor < 0) {
    ESP_LOGW(kTag, "event wake descriptor unavailable errno=%d", errno);
    return false;
  }
  wakeFd_.store(descriptor, std::memory_order_release);
  return true;
}

bool Runtime::ensureNetworkEventHandlers() {
  if (networkEventHandlersRegistered_) return true;
  esp_event_handler_instance_t wifiHandler = nullptr;
  esp_event_handler_instance_t ipHandler = nullptr;
  const esp_err_t wifiResult = esp_event_handler_instance_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, &Runtime::networkEventHandler, this,
      &wifiHandler);
  if (wifiResult != ESP_OK) return false;
  const esp_err_t ipResult = esp_event_handler_instance_register(
      IP_EVENT, ESP_EVENT_ANY_ID, &Runtime::networkEventHandler, this,
      &ipHandler);
  if (ipResult != ESP_OK) {
    (void)esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                wifiHandler);
    return false;
  }
  wifiEventHandler_ = wifiHandler;
  ipEventHandler_ = ipHandler;
  networkEventHandlersRegistered_ = true;
  return true;
}

void Runtime::removeNetworkEventHandlers() {
  if (!networkEventHandlersRegistered_) return;
  (void)esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              wifiEventHandler_);
  (void)esp_event_handler_instance_unregister(IP_EVENT, ESP_EVENT_ANY_ID,
                                              ipEventHandler_);
  wifiEventHandler_ = nullptr;
  ipEventHandler_ = nullptr;
  networkEventHandlersRegistered_ = false;
}

void Runtime::wakeIo() {
  const int descriptor = wakeFd_.load(std::memory_order_acquire);
  if (descriptor >= 0) {
    const uint64_t signal = 1;
    if (::write(descriptor, &signal, sizeof(signal)) ==
        static_cast<ssize_t>(sizeof(signal))) {
      return;
    }
  }
  if (TaskHandle_t task = ioTaskHandle_.load(std::memory_order_acquire);
      task != nullptr) {
    xTaskNotifyGive(task);
  }
}

void Runtime::waitForIo(uint32_t maximumMs) {
  const int wakeDescriptor = wakeFd_.load(std::memory_order_acquire);
  if (wakeDescriptor < 0) {
    TickType_t ticks = pdMS_TO_TICKS(maximumMs);
    if (maximumMs != 0 && ticks == 0) ticks = 1;
    (void)ulTaskNotifyTake(pdTRUE, ticks);
    return;
  }

  int socketDescriptor = -1;
  if (ioFlow_ == IoFlow::Linking &&
      linkState_ != LinkState::Backoff &&
      linkState_ != LinkState::Resolving &&
      linkState_ != LinkState::Stopped && linkState_ != LinkState::Fatal) {
    socketDescriptor = client_.fd();
  }

  fd_set reads;
  FD_ZERO(&reads);
  FD_SET(wakeDescriptor, &reads);
  int maximumDescriptor = wakeDescriptor;
  if (socketDescriptor >= 0) {
    FD_SET(socketDescriptor, &reads);
    if (socketDescriptor > maximumDescriptor) maximumDescriptor = socketDescriptor;
  }
  timeval timeout{};
  timeout.tv_sec = maximumMs / 1000;
  timeout.tv_usec = static_cast<suseconds_t>(maximumMs % 1000) * 1000;
  const int ready =
      ::select(maximumDescriptor + 1, &reads, nullptr, nullptr, &timeout);
  if (ready < 0) {
    if (errno != EINTR) vTaskDelay(1);
    return;
  }
  if (ready > 0 && FD_ISSET(wakeDescriptor, &reads)) {
    uint64_t ignored = 0;
    (void)::read(wakeDescriptor, &ignored, sizeof(ignored));
  }
}

bool Runtime::outboundPortsDirty() const {
  for (size_t i = 0; i < ports_.totalCount(); ++i) {
    const PortSlot& slot = portSlots_[i];
    if (slot.outbound && slot.wireHandle != 0 &&
        slot.outboundRevision.load(std::memory_order_acquire) !=
            slot.sentRevision) {
      return true;
    }
  }
  return false;
}

uint32_t Runtime::millisecondsUntilNextIoWork() {
  uint32_t waitMs = kIoSafetyWakeMs;
  const uint32_t now = millis();
  if (portal_.active()) {
    reduceWait(portal_.millisecondsUntilNextWork(now, waitMs), waitMs);
  }
  if (rxFrameStartedAt_ != 0) {
    reduceWait(millisecondsUntil(now, rxFrameStartedAt_ + kFrameTimeoutMs),
               waitMs);
  }

  switch (ioFlow_) {
    case IoFlow::Provisioning:
      // The protected HTTP task wakes this owner through eventfd/task notify;
      // Portal deadlines cover the deliberate 250 ms request handoff.
      break;
    case IoFlow::JoiningForEnrollment:
      reduceWait(millisecondsUntil(now, wifiDeadline_), waitMs);
      if (!networkEventHandlersRegistered_) {
        reduceWait(kWifiJoinFallbackPollMs, waitMs);
      }
      break;
    case IoFlow::JoiningForRuntime:
      reduceWait(millisecondsUntil(now, wifiDeadline_), waitMs);
      if (wifiRetryAt_ != 0) {
        reduceWait(millisecondsUntil(now, wifiRetryAt_), waitMs);
      }
      if (!networkEventHandlersRegistered_) {
        reduceWait(kWifiJoinFallbackPollMs, waitMs);
      }
      break;
    case IoFlow::Linking:
      if (linkState_ == LinkState::Resolving) {
        // CompactDns is allocation-free and nonblocking, but currently owns
        // its UDP fd privately. Preserve its 20 ms would-block retry bound.
        reduceWait(kFallbackSocketPollMs, waitMs);
      } else if (linkState_ == LinkState::Backoff) {
        reduceWait(millisecondsUntil(now, reconnectAt_), waitMs);
      } else if (linkState_ != LinkState::Fatal &&
                 linkState_ != LinkState::Stopped) {
        if (wakeFd_.load(std::memory_order_acquire) < 0 || client_.fd() < 0) {
          reduceWait(kFallbackSocketPollMs, waitMs);
        }
        if (client_.available() > 0) return 0;
        if (linkState_ == LinkState::Active) {
          if (!toolResults_.empty()) return 0;
          const uint32_t liveness =
              max(kHeartbeatTimeoutMs, heartbeatIntervalMs_ * 3u);
          reduceWait(millisecondsUntil(now, lastReceivedAt_ + liveness),
                     waitMs);
          if (heartbeatOutstanding_) {
            reduceWait(millisecondsUntil(now, lastHeartbeatAt_ + liveness),
                       waitMs);
          } else {
            reduceWait(millisecondsUntil(
                           now, lastHeartbeatAt_ + heartbeatIntervalMs_),
                       waitMs);
          }
          if (outboundPortsDirty()) {
            reduceWait(millisecondsUntil(now,
                                         lastPortFlushAt_ + kPortFlushMs),
                       waitMs);
          }
          const uint64_t serverNow = estimatedServerTimeMs();
          if (serverNow != 0) {
            for (const InFlightCall& call : inFlight_) {
              if (!call.used || call.deadlineMs == 0) continue;
              if (call.deadlineMs <= serverNow) return 0;
              const uint64_t remaining = call.deadlineMs - serverNow;
              reduceWait(static_cast<uint32_t>(
                             std::min<uint64_t>(remaining, UINT32_MAX)),
                         waitMs);
            }
          }
        } else {
          reduceWait(millisecondsUntil(
                         now, linkStateStartedAt_ + kConnectTimeoutMs),
                     waitMs);
        }
      }
      break;
    default: break;
  }
  return waitMs;
}

void Runtime::ioTask() {
  if (!initializeIo()) {
    if (ioTaskDone_ != nullptr) xSemaphoreGive(ioTaskDone_);
    for (;;) vTaskSuspend(nullptr);
  }
  ESP_LOGI(kTag,
           "I/O ready: runtime=%u bytes, stack_hwm=%u, free_heap=%u, "
           "largest=%u",
           static_cast<unsigned>(sizeof(Runtime)),
           static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)),
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
           static_cast<unsigned>(
               heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));

  while (!stopping_.load(std::memory_order_acquire)) {
    if (fatalRequested_.exchange(false, std::memory_order_acq_rel)) {
      client_.stop();
      resetSession();
      linkState_ = LinkState::Fatal;
      ioFlow_ = IoFlow::Error;
      state_.store(HampterDeviceState::Error, std::memory_order_release);
    }
    if (resetRequested_.exchange(false, std::memory_order_acq_rel)) {
      eraseEnrollment();
    }
    portal_.loop();
    if (provisioningPowerLockHeld_ && !portal_.active()) {
      releaseProvisioningPowerLock();
    }
    switch (ioFlow_) {
      case IoFlow::Provisioning:
      case IoFlow::JoiningForEnrollment: processProvisioning(); break;
      case IoFlow::JoiningForRuntime: processRuntimeWifi(); break;
      case IoFlow::Linking: linkLoop(); break;
      default: break;
    }
    waitForIo(millisecondsUntilNextIoWork());
  }

  portal_.cancelLocalResolve();
  portal_.stop();
  releaseProvisioningPowerLock();
  if (client_.connected() && linkState_ == LinkState::Active) {
    ioDocument_.clear();
    ioDocument_["code"] = 2;
    ioDocument_["message"] = "device stopping";
    (void)sendDocument(objectlink::MessageType::Goodbye,
                       objectlink::HighPriority, nextStreamId(), ioDocument_);
  }
  client_.stop();
  if (ioTaskDone_ != nullptr) xSemaphoreGive(ioTaskDone_);
  // Runtime owns this task's StaticTask_t and stack. Signal only after all
  // member access has ended, then let the destructor delete us externally
  // before freeing that storage.
  for (;;) vTaskSuspend(nullptr);
}

bool Runtime::initializeIo() {
  if (!configurePowerManagement()) {
    setError("could not activate the ESP32-C3 low-power profile", true);
    ioFlow_ = IoFlow::Error;
    return false;
  }
  (void)initializeWakeFd();
  WiFi.useStaticBuffers(true);
  WiFi.persistent(false);
  // Runtime owns reconnect timing. Leaving Arduino auto-reconnect enabled lets
  // its event task retry a stale STA config after a failed setup attempt and
  // race the next browser request.
  WiFi.setAutoReconnect(false);
  if (!store_.loadOrCreateIdentity(identity_)) {
    setError("could not load factory Setup identity", true);
    ioFlow_ = IoFlow::Error;
    return false;
  }
  if (store_.load(credentials_)) {
    publishIdentitySnapshots();
    return startRuntimeWifi();
  } else if (!startProvisioning()) {
    publishIdentitySnapshots();
    return false;
  }
  publishIdentitySnapshots();
  return true;
}

bool Runtime::startProvisioning() {
  client_.stop();
  resetSession();
  linkMode_ = LinkMode::None;
  linkState_ = LinkState::Stopped;
  if (!acquireProvisioningPowerLock() ||
      !portal_.begin(identity_, &Runtime::provisioningRadioReady,
                     &Runtime::provisioningWorkReady, this)) {
    releaseProvisioningPowerLock();
    setError("could not start protected provisioning SoftAP", true);
    ioFlow_ = IoFlow::Error;
    return false;
  }
  ioFlow_ = IoFlow::Provisioning;
  state_.store(HampterDeviceState::Provisioning, std::memory_order_release);
#if CONFIG_HAMPTER_DEVELOPMENT_LOG_SOFTAP_PASSWORD
  ESP_LOGW(kTag, "DEV SETUP ONLY: SSID=%s AP_PASSWORD=%s",
           identity_.softApSsid.c_str(), identity_.softApPassword.c_str());
#endif
  return true;
}

bool Runtime::provisioningRadioReady(void* context) {
  Runtime* runtime = static_cast<Runtime*>(context);
  if (runtime == nullptr) return false;
  if (!runtime->ensureNetworkEventHandlers()) {
    ESP_LOGW(kTag, "network event wake unavailable; using timed fallback");
  }
  (void)runtime->applyWifiPowerSave("provisioning_ap_before_beacon");
  return runtime->applyWifiTxPower("provisioning_ap_before_beacon");
}

void Runtime::provisioningWorkReady(void* context) {
  Runtime* runtime = static_cast<Runtime*>(context);
  if (runtime != nullptr) runtime->wakeIo();
}

void Runtime::processProvisioning() {
  if (ioFlow_ == IoFlow::Provisioning) {
    ProvisioningRequest request;
    if (!portal_.takeRequest(request)) return;
    clearProvisioningRequest(pendingProvisioning_);
    pendingProvisioning_ = std::move(request);
    const bool modeReady = WiFi.mode(WIFI_AP_STA);
    if (modeReady && !ensureNetworkEventHandlers()) {
      ESP_LOGW(kTag, "network event wake unavailable; using timed fallback");
    }
    if (modeReady) (void)applyWifiPowerSave("provisioning_sta_start");
    if (!modeReady || !applyWifiTxPower("provisioning_sta_start")) {
      portal_.stop();
      WiFi.disconnect(true, true);
      WiFi.mode(WIFI_OFF);
      clearProvisioningRequest(pendingProvisioning_);
      setError("Wi-Fi TX profile verification failed", true);
      ioFlow_ = IoFlow::Error;
      state_.store(HampterDeviceState::Error, std::memory_order_release);
      return;
    }
    WiFi.begin(pendingProvisioning_.wifiSsid.c_str(),
               pendingProvisioning_.wifiPassword.c_str());
    wifiDeadline_ = millis() + kWifiTimeoutMs;
    ioFlow_ = IoFlow::JoiningForEnrollment;
    state_.store(HampterDeviceState::Connecting, std::memory_order_release);
    return;
  }
  if (WiFi.status() == WL_CONNECTED) {
    portal_.setStatus(ProvisioningStatus::Enrolling,
                      "Wi-Fi connected. Verifying and enrolling with Hub…");
    clearHubBootstrap(bootstrap_);
    bootstrap_ = std::move(pendingProvisioning_.hub);
    startLink(LinkMode::Enrollment);
    ioFlow_ = IoFlow::Linking;
    return;
  }
  if (timeReached(millis(), wifiDeadline_)) {
    WiFi.disconnect(false, true);
    clearProvisioningRequest(pendingProvisioning_);
    portal_.setStatus(ProvisioningStatus::Error,
                      "Wi-Fi connection failed. Check the password and retry.");
    ioFlow_ = IoFlow::Provisioning;
    state_.store(HampterDeviceState::Provisioning, std::memory_order_release);
  }
}

bool Runtime::startRuntimeWifi() {
  if (!WiFi.mode(WIFI_STA)) {
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
    setError("could not start the Wi-Fi station radio", true);
    ioFlow_ = IoFlow::Error;
    state_.store(HampterDeviceState::Error, std::memory_order_release);
    return false;
  }
  WiFi.setHostname(identity_.deviceId.c_str());
  if (!ensureNetworkEventHandlers()) {
    ESP_LOGW(kTag, "network event wake unavailable; using timed fallback");
  }
  (void)applyWifiPowerSave("runtime_sta_start");
  if (!applyWifiTxPower("runtime_sta_start")) {
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
    setError("Wi-Fi TX profile verification failed", true);
    ioFlow_ = IoFlow::Error;
    state_.store(HampterDeviceState::Error, std::memory_order_release);
    return false;
  }
  WiFi.begin(credentials_.wifiSsid.c_str(), credentials_.wifiPassword.c_str());
  wifiDeadline_ = millis() + kWifiTimeoutMs;
  wifiRetryAt_ = 0;
  ioFlow_ = IoFlow::JoiningForRuntime;
  state_.store(HampterDeviceState::Connecting, std::memory_order_release);
  return true;
}

void Runtime::processRuntimeWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    startLink(LinkMode::Runtime);
    ioFlow_ = IoFlow::Linking;
    return;
  }
  const uint32_t now = millis();
  if (timeReached(now, wifiDeadline_) &&
      (wifiRetryAt_ == 0 || timeReached(now, wifiRetryAt_))) {
    WiFi.reconnect();
    wifiRetryAt_ = now + 5000;
    wifiDeadline_ = now + kWifiTimeoutMs;
    setError("waiting for provisioned Wi-Fi");
  }
}

bool Runtime::applyWifiTxPower(const char* phase) {
  const int8_t requested = kWifiTxPowerQuarterDbm;
  const esp_err_t setResult = esp_wifi_set_max_tx_power(requested);
  int8_t readback = INT8_MIN;
  const esp_err_t getResult = esp_wifi_get_max_tx_power(&readback);
  const bool okay = setResult == ESP_OK && getResult == ESP_OK &&
                    readback == requested;
  if (!okay) {
    ESP_LOGE(kTag,
             "TX profile failed phase=%s requested=%d set=%s get=%s "
             "readback=%d",
             phase ? phase : "unknown", requested, esp_err_to_name(setResult),
             esp_err_to_name(getResult), readback);
  } else {
    ESP_LOGI(kTag, "TX profile phase=%s raw_quarter_dbm=%d",
             phase ? phase : "unknown", requested);
  }
  return okay;
}

bool Runtime::applyWifiPowerSave(const char* phase) {
  constexpr wifi_ps_type_t requested = WIFI_PS_MIN_MODEM;
  const esp_err_t setResult = esp_wifi_set_ps(requested);
  wifi_ps_type_t readback = WIFI_PS_NONE;
  const esp_err_t getResult = esp_wifi_get_ps(&readback);
  const bool okay = setResult == ESP_OK && getResult == ESP_OK &&
                    readback == requested;
  if (!okay) {
    ESP_LOGE(kTag,
             "Wi-Fi power-save failed phase=%s requested=%d set=%s get=%s "
             "readback=%d",
             phase ? phase : "unknown", static_cast<int>(requested),
             esp_err_to_name(setResult), esp_err_to_name(getResult),
             static_cast<int>(readback));
  } else {
    ESP_LOGI(kTag, "Wi-Fi power-save phase=%s mode=min_modem",
             phase ? phase : "unknown");
  }
  return okay;
}

void Runtime::eraseEnrollment() {
  portal_.cancelLocalResolve();
  portal_.stop();
  client_.stop();
  resetSession();
  WiFi.disconnect(true, true);
  clearStoredCredentials(credentials_);
  publishIdentitySnapshots();
  clearProvisioningRequest(pendingProvisioning_);
  clearHubBootstrap(bootstrap_);
  cachedResult_ = CachedResult{};
  enrollmentCommitted_ = false;
  if (!store_.clearProvisioning() || !startProvisioning()) {
    setError("could not erase enrollment", true);
    ioFlow_ = IoFlow::Error;
  }
}

void Runtime::startLink(LinkMode mode) {
  portal_.cancelLocalResolve();
  linkMode_ = mode;
  enrollmentCommitted_ = false;
  linkState_ = LinkState::Backoff;
  reconnectAt_ = millis();
  reconnectExponent_ = 0;
  enrollmentAttempts_ = 0;
  state_.store(HampterDeviceState::Connecting, std::memory_order_release);
}

void Runtime::linkLoop() {
  if (WiFi.status() != WL_CONNECTED) {
    portal_.cancelLocalResolve();
    client_.stop();
    resetSession();
    // EnrollAck is durably committed before Register. From that point the
    // one-shot enrollment token must never be used again, even if Wi-Fi drops
    // before RegisterAck arrives.
    if (enrollmentCommitted_) {
      linkMode_ = LinkMode::Runtime;
      clearProvisioningRequest(pendingProvisioning_);
      clearHubBootstrap(bootstrap_);
    }
    if (linkMode_ == LinkMode::Runtime) {
      (void)startRuntimeWifi();
    } else {
      // A retry is supplied by a new browser POST. Do not retain the old
      // Wi-Fi password or one-shot enrollment token while waiting for it.
      WiFi.disconnect(false, true);
      clearProvisioningRequest(pendingProvisioning_);
      clearHubBootstrap(bootstrap_);
      linkMode_ = LinkMode::None;
      portal_.setStatus(ProvisioningStatus::Error,
                        "Wi-Fi disconnected during enrollment. Retry setup.");
      ioFlow_ = IoFlow::Provisioning;
      state_.store(HampterDeviceState::Provisioning,
                   std::memory_order_release);
    }
    return;
  }

  uint32_t now = millis();
  if (linkState_ == LinkState::Backoff) {
    if (timeReached(now, reconnectAt_)) attemptConnection();
    return;
  }
  if (linkState_ == LinkState::Resolving) {
    IPAddress address;
    const LocalResolveResult result = portal_.pollLocalResolve(address);
    if (result == LocalResolveResult::Pending) return;
    if (result == LocalResolveResult::Failed) {
      disconnectAndBackoff("Hub .local resolution failed");
      return;
    }
    connectTls(&address);
    return;
  }
  if (linkState_ == LinkState::Fatal || linkState_ == LinkState::Stopped) return;
  if (!client_.connected() && client_.available() <= 0) {
    disconnectAndBackoff("ObjectLink TLS socket closed");
    return;
  }

  receiveFrames();
  if (linkState_ == LinkState::Backoff || linkState_ == LinkState::Fatal) return;
  // A received handshake frame can advance the state and reset its deadline.
  // Refresh the clock before unsigned elapsed-time arithmetic so the older
  // pre-read timestamp cannot underflow against the newer state timestamp.
  now = millis();
  if (linkState_ != LinkState::Active &&
      now - linkStateStartedAt_ > kConnectTimeoutMs) {
    disconnectAndBackoff("ObjectLink handshake timed out");
    return;
  }
  if (linkState_ != LinkState::Active) return;

  expireToolCalls();
  flushToolResults();
  if (linkState_ != LinkState::Active) return;
  now = millis();
  const uint32_t liveness =
      max(kHeartbeatTimeoutMs, heartbeatIntervalMs_ * 3u);
  if (heartbeatOutstanding_ && now - lastHeartbeatAt_ > liveness) {
    disconnectAndBackoff("Heartbeat acknowledgement timed out");
    return;
  }
  if (now - lastReceivedAt_ > liveness) {
    disconnectAndBackoff("ObjectLink heartbeat timed out");
    return;
  }
  if (!heartbeatOutstanding_ &&
      now - lastHeartbeatAt_ >= heartbeatIntervalMs_ && !sendHeartbeat()) {
    disconnectAndBackoff("Heartbeat write failed");
    return;
  }
  if (now - lastPortFlushAt_ >= kPortFlushMs) (void)sendPortBatch();
}

void Runtime::attemptConnection() {
  linkStateStartedAt_ = millis();
  client_.stop();
  // Count the whole enrollment connection attempt, including DNS and
  // TCP/TLS setup. Counting only after TLS succeeds leaves the protected
  // portal permanently busy when the Hub address is unreachable.
  if (linkMode_ == LinkMode::Enrollment) ++enrollmentAttempts_;

  const String& host = linkMode_ == LinkMode::Enrollment
                           ? bootstrap_.host
                           : credentials_.hubHost;
  if (host.endsWith(".local")) {
    if (!portal_.beginLocalResolve(host.c_str(), 3000)) {
      disconnectAndBackoff("could not start Hub .local resolution");
      return;
    }
    linkState_ = LinkState::Resolving;
    return;
  }
  connectTls(nullptr);
}

void Runtime::connectTls(const IPAddress* resolvedAddress) {
  linkState_ = LinkState::Connecting;
  linkStateStartedAt_ = millis();
  client_.stop();
  client_.setInsecure();
  client_.setAlpnProtocols(kAlpn);
  client_.setHandshakeTimeout((kConnectTimeoutMs + 999) / 1000);
  client_.setConnectionTimeout(kConnectTimeoutMs);
  client_.setTimeout(kFrameTimeoutMs);

  const String& host = linkMode_ == LinkMode::Enrollment
                           ? bootstrap_.host
                           : credentials_.hubHost;
  const uint16_t port = linkMode_ == LinkMode::Enrollment
                            ? bootstrap_.port
                            : credentials_.hubPort;
  const String& fingerprint = linkMode_ == LinkMode::Enrollment
                                  ? bootstrap_.fingerprintSha256
                                  : credentials_.hubFingerprintSha256;
  const bool connected = resolvedAddress != nullptr
                             ? client_.connect(*resolvedAddress, port,
                                               host.c_str(), nullptr, nullptr,
                                               nullptr)
                             : client_.connect(host.c_str(), port);
  if (!connected) {
    disconnectAndBackoff("TLS connection failed");
    return;
  }
  client_.setConnectionTimeout(kFrameTimeoutMs);
  // setInsecure permits the self-signed Hub handshake; this pin check occurs
  // before the first ObjectLink enrollment/authentication byte is sent.
  if (!client_.verify(fingerprint.c_str(), nullptr)) {
    client_.stop();
    disconnectAndBackoff("Hub TLS fingerprint mismatch", true);
    return;
  }
  resetSession();
  linkState_ = LinkState::AwaitServerHello;
  linkStateStartedAt_ = millis();
  lastReceivedAt_ = linkStateStartedAt_;
}

void Runtime::disconnectAndBackoff(const char* reason, bool fatal) {
  portal_.cancelLocalResolve();
  client_.stop();
#if CONFIG_HAMPTER_DEVELOPMENT_LOG_SOFTAP_PASSWORD
  ESP_LOGW(kTag,
           "ObjectLink transition: reason=%s mode=%u state=%u attempt=%u "
           "committed=%u fatal=%u",
           reason ? reason : "unknown", static_cast<unsigned>(linkMode_),
           static_cast<unsigned>(linkState_),
           static_cast<unsigned>(enrollmentAttempts_),
           static_cast<unsigned>(enrollmentCommitted_),
           static_cast<unsigned>(fatal));
#endif
  setError(reason);
  resetSession();
  // Enroll tokens are one-shot. After the durable commit, every retry must
  // authenticate with the stored credential, including a failed Register.
  if (enrollmentCommitted_) linkMode_ = LinkMode::Runtime;
  if (fatal ||
      (linkMode_ == LinkMode::Enrollment && enrollmentAttempts_ >= 3)) {
    linkState_ = LinkState::Fatal;
    if (linkMode_ == LinkMode::Enrollment) {
      // Keep the protected SoftAP and browser page alive for a convenient
      // retry, but remove the joined STA before accepting new configuration.
      WiFi.disconnect(false, true);
      portal_.setStatus(ProvisioningStatus::Error, reason);
      clearHubBootstrap(bootstrap_);
      clearProvisioningRequest(pendingProvisioning_);
      linkMode_ = LinkMode::None;
      ioFlow_ = IoFlow::Provisioning;
      state_.store(HampterDeviceState::Provisioning,
                   std::memory_order_release);
    } else {
      ioFlow_ = IoFlow::Error;
      state_.store(HampterDeviceState::Error, std::memory_order_release);
    }
    return;
  }
  const uint32_t base = std::min(
      kReconnectMaxMs,
      kReconnectMinMs << std::min<uint8_t>(reconnectExponent_, 6));
  const uint32_t jitter = base > 4 ? esp_random() % (base / 4) : 0;
  reconnectAt_ = millis() + base + jitter;
  if (reconnectExponent_ < 6) ++reconnectExponent_;
  linkState_ = LinkState::Backoff;
  state_.store(HampterDeviceState::Connecting, std::memory_order_release);
}

void Runtime::resetSession() {
  sessionEpoch_.fetch_add(1, std::memory_order_acq_rel);
  for (size_t i = 0; i < CONFIG_HAMPTER_MAX_TOOLS; ++i) toolHandles_[i] = 0;
  portENTER_CRITICAL(&portMux_);
  for (size_t i = 0; i < CONFIG_HAMPTER_MAX_PORTS; ++i) {
    portSlots_[i].wireHandle = 0;
    portSlots_[i].sentRevision = 0;
    portSlots_[i].outboundSequence = 0;
    portSlots_[i].inboundSequence = 0;
    portSlots_[i].inboundPending.store(false, std::memory_order_relaxed);
  }
  portEXIT_CRITICAL(&portMux_);
  for (auto& call : inFlight_) call = InFlightCall{};
  ioDocument_.clear();
  wipeBytes(framePayload(), CONFIG_HAMPTER_FRAME_PAYLOAD_BYTES);
  rxHeaderOffset_ = 0;
  rxPayloadOffset_ = 0;
  rxFrameStartedAt_ = 0;
  negotiatedMaxPayload_ = CONFIG_HAMPTER_FRAME_PAYLOAD_BYTES;
  heartbeatIntervalMs_ = kHeartbeatDefaultMs;
  lastReceivedStreamId_ = 0;
  handshakeStreamId_ = 0;
  serverTimeBaseMs_ = 0;
  serverTimeBaseLocalMs_ = 0;
  lastHeartbeatMonotonicMs_ = 0;
  lastHeartbeatAckedMonotonicMs_ = 0;
  heartbeatOutstanding_ = false;
}

void Runtime::receiveFrames() {
  uint8_t frames = 0;
  size_t bytesProcessed = 0;
  while (frames < kFramesPerStep && bytesProcessed < kBytesPerStep) {
    int available = client_.available();
    if (available <= 0) break;
    if (rxFrameStartedAt_ == 0) rxFrameStartedAt_ = millis();
    if (rxHeaderOffset_ < sizeof(rxHeader_)) {
      const size_t wanted =
          std::min<size_t>(available, sizeof(rxHeader_) - rxHeaderOffset_);
      const int received = client_.read(rxHeader_ + rxHeaderOffset_, wanted);
      if (received <= 0) break;
      rxHeaderOffset_ += received;
      bytesProcessed += received;
      if (rxHeaderOffset_ < sizeof(rxHeader_)) continue;
      if (objectlink::decodeHeader(rxHeader_, negotiatedMaxPayload_,
                                   rxDecodedHeader_) !=
          objectlink::HeaderError::None) {
        disconnectAndBackoff("invalid ObjectLink frame header", true);
        return;
      }
    }
    if (rxPayloadOffset_ < rxDecodedHeader_.payloadLength) {
      available = client_.available();
      if (available <= 0) break;
      const size_t wanted = std::min<size_t>(
          available, rxDecodedHeader_.payloadLength - rxPayloadOffset_);
      const int received =
          client_.read(framePayload() + rxPayloadOffset_, wanted);
      if (received <= 0) break;
      rxPayloadOffset_ += received;
      bytesProcessed += received;
      if (rxPayloadOffset_ < rxDecodedHeader_.payloadLength) continue;
    }
    const size_t completedPayloadLength = rxDecodedHeader_.payloadLength;
    const bool processed = processFrame(rxDecodedHeader_, framePayload(),
                                        completedPayloadLength);
    wipeBytes(framePayload(), completedPayloadLength);
    if (!processed) {
#if CONFIG_HAMPTER_DEVELOPMENT_LOG_SOFTAP_PASSWORD
      ESP_LOGE(kTag,
               "ObjectLink frame rejected: type=%u state=%u payload_bytes=%u",
               static_cast<unsigned>(rxDecodedHeader_.type),
               static_cast<unsigned>(linkState_),
               static_cast<unsigned>(completedPayloadLength));
#endif
      disconnectAndBackoff("invalid ObjectLink message", true);
      return;
    }
    if (linkState_ == LinkState::Backoff ||
        linkState_ == LinkState::Fatal ||
        linkState_ == LinkState::Stopped) {
      return;
    }
    lastReceivedAt_ = millis();
    if (rxDecodedHeader_.streamId != 0) {
      lastReceivedStreamId_ = rxDecodedHeader_.streamId;
    }
    rxHeaderOffset_ = 0;
    rxPayloadOffset_ = 0;
    rxFrameStartedAt_ = 0;
    ++frames;
  }
  if (rxFrameStartedAt_ != 0 && millis() - rxFrameStartedAt_ > kFrameTimeoutMs) {
    disconnectAndBackoff("partial ObjectLink frame timed out");
  }
}

bool Runtime::processFrame(const objectlink::Header& header,
                           const uint8_t* payload, size_t length) {
  ioDocument_.clear();
  if (length == 0 ||
      deserializeMsgPack(ioDocument_, payload, length,
                         DeserializationOption::NestingLimit(10)) ||
      !ioDocument_.is<JsonObject>()) {
    return false;
  }
  if (header.type == objectlink::MessageType::Error) {
    const char* message = ioDocument_["message"] | "Hub protocol error";
    const bool fatal = ioDocument_["fatal"] | true;
    if (fatal) disconnectAndBackoff(message, true);
    else setError(message);
    return true;
  }
  if (header.type == objectlink::MessageType::Goodbye) {
    disconnectAndBackoff(ioDocument_["message"] | "Hub closed the session");
    return true;
  }
  switch (linkState_) {
    case LinkState::AwaitServerHello:
      return header.type == objectlink::MessageType::ServerHello &&
             processServerHello(header, ioDocument_);
    case LinkState::AwaitEnrollment:
      return header.type == objectlink::MessageType::EnrollAck &&
             processEnrollAck(header, ioDocument_);
    case LinkState::AwaitAuthentication:
      return header.type == objectlink::MessageType::AuthAck &&
             processAuthAck(header, ioDocument_);
    case LinkState::AwaitRegistration:
      return header.type == objectlink::MessageType::RegisterAck &&
             processRegisterAck(header, ioDocument_);
    case LinkState::Active:
      switch (header.type) {
        case objectlink::MessageType::HeartbeatAck: {
          const uint64_t echoed = ioDocument_["echo_monotonic_ms"] | 0ULL;
          const uint64_t serverTime = ioDocument_["server_time_ms"] | 0ULL;
          if (echoed == 0) return false;
          if (echoed == lastHeartbeatAckedMonotonicMs_) return true;
          if (!heartbeatOutstanding_ || echoed != lastHeartbeatMonotonicMs_) {
            return false;
          }
          heartbeatOutstanding_ = false;
          lastHeartbeatAckedMonotonicMs_ = echoed;
          if (serverTime != 0) {
            serverTimeBaseMs_ = serverTime;
            serverTimeBaseLocalMs_ = monotonicMs();
          }
          return true;
        }
        case objectlink::MessageType::Heartbeat: {
          const uint64_t echo = ioDocument_["monotonic_ms"] | 0ULL;
          ioDocument_.clear();
          ioDocument_["echo_monotonic_ms"] = echo;
          ioDocument_["server_time_ms"] = estimatedServerTimeMs();
          if (!sendDocument(objectlink::MessageType::HeartbeatAck,
                            objectlink::HighPriority, header.streamId,
                            ioDocument_)) {
            disconnectAndBackoff("HeartbeatAck write failed");
          }
          return true;
        }
        case objectlink::MessageType::PortBatch:
          return processPortBatch(ioDocument_);
        case objectlink::MessageType::ToolDispatch:
          if (!processToolDispatch(header, ioDocument_)) {
            disconnectAndBackoff("Tool acknowledgement write failed");
          }
          return true;
        default: return false;
      }
    default: return false;
  }
}

bool Runtime::processServerHello(const objectlink::Header& header,
                                 JsonDocument& document) {
  const MsgPackBinary nonce = document["server_nonce"].as<MsgPackBinary>();
  const uint32_t maximum = document["max_payload_bytes"] | 0U;
  if ((document["protocol_version"] | 0) != objectlink::kVersion ||
      nonce.size() != 32 || maximum < 256) {
    return false;
  }
  negotiatedMaxPayload_ =
      std::min<uint32_t>(maximum, CONFIG_HAMPTER_FRAME_PAYLOAD_BYTES);
  heartbeatIntervalMs_ = std::clamp<uint32_t>(
      document["heartbeat_interval_ms"] | kHeartbeatDefaultMs, 5000, 120000);
  serverTimeBaseMs_ = document["server_time_ms"] | 0ULL;
  serverTimeBaseLocalMs_ = monotonicMs();
  lastReceivedStreamId_ = header.streamId;
  if (linkMode_ == LinkMode::Enrollment) {
    if (!(document["enrollment_supported"] | false)) return false;
    if (!sendEnrollment()) disconnectAndBackoff("Enroll write failed");
    return true;
  }
  if (linkMode_ != LinkMode::Runtime) return false;
  if (credentials_.credentialExpiresAtMs != 0 && serverTimeBaseMs_ != 0 &&
      credentials_.credentialExpiresAtMs <= serverTimeBaseMs_) {
    disconnectAndBackoff("stored device credential expired", true);
    return true;
  }
  if (!sendAuthentication()) {
    disconnectAndBackoff("Authenticate write failed");
  }
  return true;
}

bool Runtime::sendEnrollment() {
  uint8_t publicKey[160];
  size_t publicKeyLength = 0;
  if (!generateP256PublicKey(publicKey, sizeof(publicKey), publicKeyLength)) {
    return false;
  }
  WipingHeapAllocator allocator;
  JsonDocument document(&allocator);
  document["enrollment_token"] = MsgPackBinary(
      bootstrap_.enrollmentToken.c_str(), bootstrap_.enrollmentToken.length());
  document["device_public_key"] =
      MsgPackBinary(publicKey, publicKeyLength);
  document["device_id_hint"] = identity_.deviceId;
  const size_t payloadLength = measureMsgPack(document);
  if (payloadLength == 0 ||
      payloadLength > CONFIG_HAMPTER_FRAME_PAYLOAD_BYTES) {
    return false;
  }
  ByteWiper payloadWiper(framePayload(), payloadLength);
  handshakeStreamId_ = nextStreamId();
  if (!sendDocument(objectlink::MessageType::Enroll,
                     objectlink::AckRequired | objectlink::HighPriority,
                     handshakeStreamId_, document)) {
    return false;
  }
  linkState_ = LinkState::AwaitEnrollment;
  linkStateStartedAt_ = millis();
  return true;
}

bool Runtime::processEnrollAck(const objectlink::Header& header,
                               JsonDocument& document) {
  if (header.streamId != handshakeStreamId_) {
#if CONFIG_HAMPTER_DEVELOPMENT_LOG_SOFTAP_PASSWORD
    ESP_LOGE(kTag, "EnrollAck stream mismatch");
#endif
    return false;
  }
  const char* objectId = document["object_id"] | "";
  const MsgPackBinary credential = document["credential"].as<MsgPackBinary>();
  const MsgPackBinary fingerprint =
      document["hub_fingerprint"].as<MsgPackBinary>();
  JsonVariantConst expiresAtValue = document["credential_expires_at_ms"];
  if (!expiresAtValue.isNull() && !expiresAtValue.is<uint64_t>()) {
#if CONFIG_HAMPTER_DEVELOPMENT_LOG_SOFTAP_PASSWORD
    ESP_LOGE(kTag, "EnrollAck expiry has an invalid wire type");
#endif
    return false;
  }
  const uint64_t expiresAt = expiresAtValue | 0ULL;
  const bool objectIdValid = validIdentifier(objectId);
  const bool credentialValid =
      credential.size() >= 16 && credential.size() <= 256;
  const bool fingerprintLengthValid = fingerprint.size() == 32;
  const bool fingerprintValid =
      fingerprintLengthValid &&
      fingerprintMatches(
          bootstrap_.fingerprintSha256,
          static_cast<const uint8_t*>(fingerprint.data()), fingerprint.size());
  if (!objectIdValid || !credentialValid || !fingerprintValid) {
#if CONFIG_HAMPTER_DEVELOPMENT_LOG_SOFTAP_PASSWORD
    ESP_LOGE(kTag,
             "EnrollAck validation failed: object_id=%u credential_bytes=%u "
             "fingerprint_bytes=%u fingerprint_match=%u",
             static_cast<unsigned>(objectIdValid),
             static_cast<unsigned>(credential.size()),
             static_cast<unsigned>(fingerprint.size()),
             static_cast<unsigned>(fingerprintValid));
#endif
    return false;
  }
  const uint64_t serverNow = estimatedServerTimeMs();
  if (expiresAt != 0 && serverNow != 0 && expiresAt <= serverNow) {
#if CONFIG_HAMPTER_DEVELOPMENT_LOG_SOFTAP_PASSWORD
    ESP_LOGE(kTag, "EnrollAck credential is already expired");
#endif
    return false;
  }
  StoredCredentials enrolled;
  StoredCredentialSecretGuard enrolledSecrets(enrolled);
  enrolled.wifiSsid = pendingProvisioning_.wifiSsid;
  enrolled.wifiPassword = pendingProvisioning_.wifiPassword;
  enrolled.hubHost = bootstrap_.host;
  enrolled.hubPort = bootstrap_.port;
  enrolled.hubFingerprintSha256 = bootstrap_.fingerprintSha256;
  enrolled.objectId = objectId;
  enrolled.deviceCredential = hexEncode(
      static_cast<const uint8_t*>(credential.data()), credential.size());
  enrolled.credentialExpiresAtMs = expiresAt;
  const bool recordValid = enrolled.valid();
  const bool saved = recordValid && store_.save(enrolled);
  const bool verified = saved && store_.load(credentials_);
  if (!verified) {
#if CONFIG_HAMPTER_DEVELOPMENT_LOG_SOFTAP_PASSWORD
    ESP_LOGE(kTag,
             "EnrollAck durable commit failed: record=%u save=%u verify=%u",
             static_cast<unsigned>(recordValid), static_cast<unsigned>(saved),
             static_cast<unsigned>(verified));
#endif
    // This status accepts a fresh provisioning request, so expose it only
    // after the destination STA has been removed.
    WiFi.disconnect(false, true);
    portal_.setStatus(ProvisioningStatus::Error,
                      "Enrollment succeeded but credential storage failed.");
    return false;
  }
#if CONFIG_HAMPTER_DEVELOPMENT_LOG_SOFTAP_PASSWORD
  ESP_LOGI(kTag, "EnrollAck credential committed to NVS");
#endif
  publishIdentitySnapshots();
  wipeStringSecret(bootstrap_.enrollmentToken);
  wipeStringSecret(pendingProvisioning_.wifiPassword);
  enrollmentCommitted_ = true;
  // Durable enrollment is the one user-visible setup boundary. From here on,
  // transient Hub or registration failures use the stored credential and
  // reconnect automatically; the user never needs to provision again.
  linkMode_ = LinkMode::Runtime;
  portal_.setStatus(
      ProvisioningStatus::Complete,
      "Setup saved. This device will keep connecting automatically.");
  if (!sendRegistration()) {
    disconnectAndBackoff("Register write failed after enrollment");
  }
  return true;
}

bool Runtime::sendAuthentication() {
  uint8_t credential[256];
  ByteWiper credentialWiper(credential, sizeof(credential));
  size_t credentialLength = 0;
  if (!hexDecode(credentials_.deviceCredential, credential,
                 sizeof(credential), credentialLength) ||
      credentialLength < 16) {
    return false;
  }
  uint8_t nonce[32];
  ByteWiper nonceWiper(nonce, sizeof(nonce));
  randomBytes(nonce, sizeof(nonce));
  WipingHeapAllocator allocator;
  JsonDocument document(&allocator);
  document["object_id"] = credentials_.objectId;
  document["credential"] = MsgPackBinary(credential, credentialLength);
  document["client_nonce"] = MsgPackBinary(nonce, sizeof(nonce));
  const size_t payloadLength = measureMsgPack(document);
  if (payloadLength == 0 ||
      payloadLength > CONFIG_HAMPTER_FRAME_PAYLOAD_BYTES) {
    return false;
  }
  ByteWiper payloadWiper(framePayload(), payloadLength);
  handshakeStreamId_ = nextStreamId();
  if (!sendDocument(objectlink::MessageType::Authenticate,
                     objectlink::AckRequired | objectlink::HighPriority,
                     handshakeStreamId_, document)) {
    return false;
  }
  linkState_ = LinkState::AwaitAuthentication;
  linkStateStartedAt_ = millis();
  return true;
}

bool Runtime::processAuthAck(const objectlink::Header& header,
                             JsonDocument& document) {
  if (header.streamId != handshakeStreamId_) return false;
  const MsgPackBinary session = document["session_id"].as<MsgPackBinary>();
  const MsgPackBinary nonce = document["server_nonce"].as<MsgPackBinary>();
  if (session.size() != 16 || nonce.size() != 32) return false;
  heartbeatIntervalMs_ = std::clamp<uint32_t>(
      document["heartbeat_interval_ms"] | heartbeatIntervalMs_, 5000, 120000);
  if (!sendRegistration()) disconnectAndBackoff("Register write failed");
  return true;
}

bool Runtime::sendRegistration() {
  ioDocument_.clear();
  ioDocument_["client_max_payload_bytes"] =
      CONFIG_HAMPTER_FRAME_PAYLOAD_BYTES;
  JsonObject limits = ioDocument_["receive_limits"].to<JsonObject>();
  limits["max_tool_argument_bytes"] = CONFIG_HAMPTER_TOOL_ARGUMENT_MAX_BYTES;
  limits["max_tool_result_bytes"] = CONFIG_HAMPTER_TOOL_RESULT_MAX_BYTES;
  limits["max_port_value_bytes"] = 16;
  JsonObject object = ioDocument_["object"].to<JsonObject>();
  object["object_id"] = credentials_.objectId;
  object["display_name"] = config_.name;
  object["icon"] = config_.icon;
  object["manifest"].set(manifestDocument_.as<JsonVariantConst>());
  handshakeStreamId_ = nextStreamId();
  if (!sendDocument(objectlink::MessageType::Register,
                    objectlink::AckRequired | objectlink::HighPriority,
                    handshakeStreamId_, ioDocument_)) {
    return false;
  }
  linkState_ = LinkState::AwaitRegistration;
  linkStateStartedAt_ = millis();
  return true;
}

bool Runtime::processRegisterAck(const objectlink::Header& header,
                                 JsonDocument& document) {
  if (header.streamId != handshakeStreamId_) return false;
  JsonArrayConst tools = document["tool_handles"].as<JsonArrayConst>();
  JsonArrayConst ports = document["port_handles"].as<JsonArrayConst>();
  if (tools.size() != tools_.count() || ports.size() != ports_.totalCount()) {
    return false;
  }
  uint16_t used[CONFIG_HAMPTER_MAX_TOOLS + CONFIG_HAMPTER_MAX_PORTS]{};
  size_t usedCount = 0;
  for (JsonObjectConst binding : tools) {
    const char* name = binding["name"] | "";
    const uint16_t handle = binding["handle"] | 0;
    int local = -1;
    for (size_t i = 0; i < tools_.count(); ++i) {
      if (strcmp(tools_.at(i)->name(), name) == 0) local = i;
    }
    if (local < 0 || handle == 0 || toolHandles_[local] != 0) return false;
    for (size_t i = 0; i < usedCount; ++i) {
      if (used[i] == handle) return false;
    }
    used[usedCount++] = handle;
    toolHandles_[local] = handle;
  }
  for (JsonObjectConst binding : ports) {
    const char* name = binding["name"] | "";
    const char* direction = binding["direction"] | "";
    const PortRegistry::Direction expected =
        strcmp(direction, "in") == 0 ? PortRegistry::Direction::In
                                      : PortRegistry::Direction::Out;
    if (strcmp(direction, "in") != 0 && strcmp(direction, "out") != 0) {
      return false;
    }
    const int local = ports_.findEntry(name, expected);
    const uint16_t handle = binding["handle"] | 0;
    if (local < 0 || handle == 0 || portSlots_[local].wireHandle != 0) {
      return false;
    }
    for (size_t i = 0; i < usedCount; ++i) {
      if (used[i] == handle) return false;
    }
    used[usedCount++] = handle;
    portSlots_[local].wireHandle = handle;
  }
  for (size_t i = 0; i < tools_.count(); ++i) {
    if (toolHandles_[i] == 0) return false;
  }
  for (size_t i = 0; i < ports_.totalCount(); ++i) {
    if (portSlots_[i].wireHandle == 0) return false;
    portSlots_[i].inboundSequence = 0;
    portENTER_CRITICAL(&portMux_);
    portSlots_[i].inboundPending.store(false, std::memory_order_relaxed);
    portEXIT_CRITICAL(&portMux_);
    portSlots_[i].sentRevision = 0;
    portSlots_[i].outboundSequence = 0;
  }
  reconnectExponent_ = 0;
  linkState_ = LinkState::Active;
  linkStateStartedAt_ = millis();
  lastReceivedAt_ = linkStateStartedAt_;
  lastHeartbeatAt_ = linkStateStartedAt_;
  heartbeatOutstanding_ = false;
  lastHeartbeatAckedMonotonicMs_ = 0;
  lastPortFlushAt_ = linkStateStartedAt_;
  state_.store(HampterDeviceState::Online, std::memory_order_release);
  setError("");
  if (linkMode_ == LinkMode::Enrollment || enrollmentCommitted_) {
    portal_.setStatus(ProvisioningStatus::Complete,
                      "Enrollment complete. Device is online.");
    linkMode_ = LinkMode::Runtime;
    clearHubBootstrap(bootstrap_);
    clearProvisioningRequest(pendingProvisioning_);
    enrollmentCommitted_ = false;
  }
  return true;
}

bool Runtime::sendHeartbeat() {
  ioDocument_.clear();
  const uint64_t sentAt = monotonicMs();
  ioDocument_["monotonic_ms"] = sentAt;
  ioDocument_["last_received_stream_id"] = lastReceivedStreamId_;
  const bool sent = sendDocument(
      objectlink::MessageType::Heartbeat,
      objectlink::AckRequired | objectlink::HighPriority, nextStreamId(),
      ioDocument_);
  if (sent) {
    lastHeartbeatAt_ = millis();
    lastHeartbeatMonotonicMs_ = sentAt;
    heartbeatOutstanding_ = true;
  }
  return sent;
}

bool Runtime::sendPortBatch() {
  bool dirty = false;
  for (size_t i = 0; i < ports_.totalCount(); ++i) {
    const PortSlot& slot = portSlots_[i];
    if (slot.outbound && slot.wireHandle != 0 &&
        slot.outboundRevision.load(std::memory_order_acquire) !=
            slot.sentRevision) {
      dirty = true;
      break;
    }
  }
  lastPortFlushAt_ = millis();
  if (!dirty) return true;

  ioDocument_.clear();
  JsonArray samples = ioDocument_["samples"].to<JsonArray>();
  uint16_t included[CONFIG_HAMPTER_MAX_PORTS]{};
  uint32_t revisions[CONFIG_HAMPTER_MAX_PORTS]{};
  uint64_t sequences[CONFIG_HAMPTER_MAX_PORTS]{};
  size_t count = 0;
  for (size_t i = 0; i < ports_.totalCount(); ++i) {
    PortSlot& slot = portSlots_[i];
    if (!slot.outbound || slot.wireHandle == 0) continue;
    uint32_t before = 0;
    uint32_t after = 0;
    uint32_t bits = 0;
    do {
      before = slot.outboundRevision.load(std::memory_order_acquire);
      bits = slot.outboundBits.load(std::memory_order_relaxed);
      after = slot.outboundRevision.load(std::memory_order_acquire);
    } while (before != after);
    if (after == 0 || after == slot.sentRevision) continue;
    JsonObject sample = samples.add<JsonObject>();
    sample["port_handle"] = slot.wireHandle;
    uint64_t sequence = slot.outboundSequence + 1;
    if (sequence == 0) sequence = 1;
    sample["sequence"] = sequence;
    sample["value"] = static_cast<double>(bitsFloat(bits));
    if (measureMsgPack(ioDocument_) > negotiatedMaxPayload_) {
      samples.remove(samples.size() - 1);
      break;
    }
    included[count] = i;
    revisions[count] = after;
    sequences[count] = sequence;
    ++count;
  }
  if (count == 0) return true;
  if (!sendDocument(objectlink::MessageType::PortBatch, 0, nextStreamId(),
                    ioDocument_)) {
    disconnectAndBackoff("PortBatch write failed");
    return false;
  }
  for (size_t i = 0; i < count; ++i) {
    PortSlot& slot = portSlots_[included[i]];
    slot.outboundSequence = sequences[i];
    if (slot.outboundRevision.load(std::memory_order_acquire) == revisions[i]) {
      slot.sentRevision = revisions[i];
    }
  }
  return true;
}

bool Runtime::processPortBatch(JsonDocument& document) {
  JsonArrayConst samples = document["samples"].as<JsonArrayConst>();
  if (samples.size() == 0 || samples.size() > ports_.totalCount()) return false;
  bool seen[CONFIG_HAMPTER_MAX_PORTS]{};
  uint16_t locals[CONFIG_HAMPTER_MAX_PORTS]{};
  uint64_t sequences[CONFIG_HAMPTER_MAX_PORTS]{};
  uint32_t values[CONFIG_HAMPTER_MAX_PORTS]{};
  size_t count = 0;
  for (JsonObjectConst sample : samples) {
    const uint16_t handle = sample["port_handle"] | 0;
    const int local = findPortByHandle(handle);
    if (handle == 0 || local < 0 || seen[local] || portSlots_[local].outbound ||
        !sample["value"].is<double>()) {
      return false;
    }
    seen[local] = true;
    const uint64_t sequence = sample["sequence"] | 0ULL;
    const double value = sample["value"].as<double>();
    if (sequence == 0 || !isfinite(value) || fabs(value) > FLT_MAX) {
      return false;
    }
    locals[count] = static_cast<uint16_t>(local);
    sequences[count] = sequence;
    values[count] = floatBits(static_cast<float>(value));
    ++count;
  }

  // Commit only after every sample has passed validation. The app task can
  // consume pending values concurrently, so partial commit is not reversible.
  bool appChanged = false;
  for (size_t i = 0; i < count; ++i) {
    PortSlot& slot = portSlots_[locals[i]];
    if (sequences[i] <= slot.inboundSequence) continue;
    slot.inboundSequence = sequences[i];
    portENTER_CRITICAL(&portMux_);
    slot.inboundBits.store(values[i], std::memory_order_relaxed);
    slot.inboundPending.store(true, std::memory_order_relaxed);
    portEXIT_CRITICAL(&portMux_);
    appChanged = true;
  }
  if (appChanged) wakeApp();
  return true;
}

bool Runtime::processToolDispatch(const objectlink::Header& header,
                                  JsonDocument& document) {
  const uint64_t callId = document["call_id"] | 0ULL;
  const uint16_t handle = document["tool_handle"] | 0;
  const uint64_t deadline = document["deadline_ms"] | 0ULL;
  const int toolIndex = findToolByHandle(handle);
  if (callId == 0 || handle == 0 || toolIndex < 0 ||
      !document["arguments"].is<JsonObject>()) {
    return sendToolAck(header.streamId, callId, "rejected");
  }
  if (findInFlight(callId) != nullptr) {
    return sendToolAck(header.streamId, callId, "accepted");
  }
  if (cachedResult_.valid && cachedResult_.callId == callId) {
    return sendToolAck(header.streamId, callId, "accepted") &&
           sendFrame(objectlink::MessageType::ToolResult,
                     objectlink::HighPriority, header.streamId,
                     cachedResult_.payload, cachedResult_.length);
  }
  const uint64_t serverNow = estimatedServerTimeMs();
  if (deadline != 0 && serverNow != 0 && deadline <= serverNow) {
    return sendToolAck(header.streamId, callId, "rejected");
  }
  const size_t argumentLength = measureMsgPack(document["arguments"]);
  if (argumentLength == 0 ||
      argumentLength > CONFIG_HAMPTER_TOOL_ARGUMENT_MAX_BYTES ||
      toolJobs_.full() || !toolArguments_.canPush(argumentLength)) {
    return sendToolAck(header.streamId, callId, "busy", kBusyRetryMs);
  }
  if (serializeMsgPack(document["arguments"], framePayload(),
                       CONFIG_HAMPTER_FRAME_PAYLOAD_BYTES) != argumentLength) {
    return sendToolAck(header.streamId, callId, "rejected");
  }
  ToolJob job;
  job.callId = callId;
  job.deadlineMs = deadline;
  job.streamId = header.streamId;
  job.epoch = sessionEpoch_.load(std::memory_order_acquire);
  job.toolIndex = toolIndex;
  job.argumentLength = argumentLength;
  if (deadline != 0 && serverNow != 0 && deadline > serverNow) {
    const uint64_t remaining = std::min<uint64_t>(deadline - serverNow,
                                                  INT32_MAX - 1u);
    job.localDeadlineMs = millis() + static_cast<uint32_t>(remaining);
  }
  if (allocateInFlight(job) == nullptr ||
      !toolArguments_.tryPush(framePayload(), argumentLength) ||
      !toolJobs_.tryPush(job)) {
    releaseInFlight(callId);
    return sendToolAck(header.streamId, callId, "busy", kBusyRetryMs);
  }
  wakeApp();
  return sendToolAck(header.streamId, callId, "accepted");
}

bool Runtime::sendToolAck(uint32_t streamId, uint64_t callId,
                          const char* disposition, uint32_t retryAfterMs) {
  ioDocument_.clear();
  ioDocument_["call_id"] = callId;
  ioDocument_["state"] = disposition;
  if (retryAfterMs != 0) ioDocument_["retry_after_ms"] = retryAfterMs;
  return sendDocument(objectlink::MessageType::ToolAck,
                      objectlink::HighPriority, streamId, ioDocument_);
}

bool Runtime::sendToolFailure(uint32_t streamId, uint64_t callId,
                              const char* outcome, const char* code,
                              const char* message, bool retryable) {
  ioDocument_.clear();
  ioDocument_["call_id"] = callId;
  ioDocument_["outcome"] = outcome;
  JsonObject error = ioDocument_["error"].to<JsonObject>();
  error["code"] = code;
  error["message"] = message;
  error["retryable"] = retryable;
  const size_t length = measureMsgPack(ioDocument_);
  const bool sent = sendDocument(objectlink::MessageType::ToolResult,
                                 objectlink::HighPriority, streamId,
                                 ioDocument_);
  if (sent && length > 0 && length <= sizeof(cachedResult_.payload)) {
    cachedResult_.valid = true;
    cachedResult_.callId = callId;
    cachedResult_.length = static_cast<uint16_t>(length);
    memcpy(cachedResult_.payload, framePayload(), length);
  }
  return sent;
}

bool Runtime::sendFrame(objectlink::MessageType type, uint16_t flags,
                        uint32_t streamId, const uint8_t* payload,
                        size_t length) {
  if (!client_.connected() || length > negotiatedMaxPayload_ ||
      length > CONFIG_HAMPTER_FRAME_PAYLOAD_BYTES ||
      (payload == nullptr && length != 0)) {
    return false;
  }
  objectlink::Header header;
  header.type = type;
  header.flags = flags;
  header.streamId = streamId;
  header.payloadLength = length;
  if (payload != framePayload() && length != 0) {
    memmove(framePayload(), payload, length);
  }
  objectlink::encodeHeader(header, frameBuffer_);
  const uint32_t started = millis();
  auto writeAll = [&](const uint8_t* bytes, size_t count) {
    size_t offset = 0;
    while (offset < count && client_.connected()) {
      const size_t written = client_.write(bytes + offset, count - offset);
      if (written == 0) {
        if (millis() - started > kFrameTimeoutMs) return false;
        vTaskDelay(1);
      } else {
        offset += written;
      }
    }
    return offset == count;
  };
  return writeAll(frameBuffer_, objectlink::kHeaderSize + length);
}

bool Runtime::sendDocument(objectlink::MessageType type, uint16_t flags,
                           uint32_t streamId, JsonDocument& document) {
  const size_t length = measureMsgPack(document);
  if (length == 0 || length > negotiatedMaxPayload_ ||
      length > CONFIG_HAMPTER_FRAME_PAYLOAD_BYTES || document.overflowed() ||
      serializeMsgPack(document, framePayload(),
                       CONFIG_HAMPTER_FRAME_PAYLOAD_BYTES) !=
          length) {
    return false;
  }
  return sendFrame(type, flags, streamId, framePayload(), length);
}

void Runtime::flushToolResults() {
  if (linkState_ != LinkState::Active) return;
  ToolResultRecord result;
  if (!toolResults_.tryPop(result)) return;
  size_t length = 0;
  if (!toolResultBytes_.tryPop(framePayload(),
                               CONFIG_HAMPTER_FRAME_PAYLOAD_BYTES, length) ||
      length != result.payloadLength) {
    disconnectAndBackoff("Tool result byte-pool order violation", true);
    return;
  }
  wakeApp();
  if (result.epoch != sessionEpoch_.load(std::memory_order_acquire)) return;
  InFlightCall* call = findInFlight(result.callId);
  if (call == nullptr || call->epoch != result.epoch) return;
  if (!sendFrame(objectlink::MessageType::ToolResult,
                  objectlink::HighPriority, result.streamId, framePayload(),
                 length)) {
    disconnectAndBackoff("ToolResult write failed");
    return;
  }
  cachedResult_.valid = true;
  cachedResult_.callId = result.callId;
  cachedResult_.length = length;
  memcpy(cachedResult_.payload, framePayload(), length);
  releaseInFlight(result.callId);
}

void Runtime::expireToolCalls() {
  const uint64_t now = estimatedServerTimeMs();
  if (now == 0) return;
  for (auto& call : inFlight_) {
    if (!call.used || call.deadlineMs == 0 || now < call.deadlineMs) continue;
    if (!sendToolFailure(call.streamId, call.callId, "timed_out",
                         "deadline_exceeded",
                         "Tool deadline expired on device", false)) {
      disconnectAndBackoff("timed-out Tool result write failed");
      return;
    }
    call = InFlightCall{};
  }
}

Runtime::InFlightCall* Runtime::findInFlight(uint64_t callId) {
  for (auto& call : inFlight_) {
    if (call.used && call.callId == callId) return &call;
  }
  return nullptr;
}

Runtime::InFlightCall* Runtime::allocateInFlight(const ToolJob& job) {
  for (auto& call : inFlight_) {
    if (!call.used) {
      call.used = true;
      call.callId = job.callId;
      call.deadlineMs = job.deadlineMs;
      call.streamId = job.streamId;
      call.epoch = job.epoch;
      return &call;
    }
  }
  return nullptr;
}

void Runtime::releaseInFlight(uint64_t callId) {
  if (InFlightCall* call = findInFlight(callId)) *call = InFlightCall{};
}

int Runtime::findToolByHandle(uint16_t handle) const {
  if (handle == 0) return -1;
  for (size_t i = 0; i < tools_.count(); ++i) {
    if (toolHandles_[i] == handle) return i;
  }
  return -1;
}

int Runtime::findPortByHandle(uint16_t handle) const {
  if (handle == 0) return -1;
  for (size_t i = 0; i < ports_.totalCount(); ++i) {
    if (portSlots_[i].wireHandle == handle) return i;
  }
  return -1;
}

uint32_t Runtime::nextStreamId() {
  ++streamCounter_;
  if (streamCounter_ == 0) ++streamCounter_;
  return streamCounter_;
}

uint64_t Runtime::estimatedServerTimeMs() const {
  return serverTimeBaseMs_ == 0
             ? 0
             : serverTimeBaseMs_ + (monotonicMs() - serverTimeBaseLocalMs_);
}

void Runtime::setError(const char* reason, bool fatal) {
  portENTER_CRITICAL(&errorMux_);
  if (!fatal && fatalRequested_.load(std::memory_order_relaxed)) {
    portEXIT_CRITICAL(&errorMux_);
    return;
  }
  if (fatal) fatalRequested_.store(true, std::memory_order_relaxed);
  copyText(reason ? reason : "runtime error", lastError_);
  portEXIT_CRITICAL(&errorMux_);
  if (fatal) {
    state_.store(HampterDeviceState::Error, std::memory_order_release);
    if (TaskHandle_t task = ioTaskHandle_.load(std::memory_order_acquire);
        task != nullptr && task != xTaskGetCurrentTaskHandle()) {
      wakeIo();
    }
  }
}

}  // namespace hampter::internal
