#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFiClientSecure.h>
#include <esp_event.h>
#include <esp_pm.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>
#include <stddef.h>

#include "CredentialStore.h"
#include "HampterDevice.h"
#include "ProvisioningPortal.h"
#include "hampter/internal/ObjectLinkProtocol.h"
#include "hampter/internal/SpscByteRing.h"
#include "hampter/internal/SpscRing.h"

namespace hampter::internal {

class Runtime {
 public:
  Runtime(const HampterDeviceConfig& config, ToolRegistry& tools,
          PortRegistry& ports);
  ~Runtime();

  bool begin();
  void loop();
  HampterDeviceState state() const {
    return state_.load(std::memory_order_acquire);
  }
  bool online() const { return state() == HampterDeviceState::Online; }
  const char* lastError() const;
  bool copyDeviceId(char* output, size_t capacity) const;
  bool copyObjectId(char* output, size_t capacity) const;
  bool publishPort(const char* name, float value);
  void waitForAppWork(uint32_t maximumMs);

 private:
  static_assert(
      CONFIG_HAMPTER_TOOL_BYTE_POOL_BYTES >=
          CONFIG_HAMPTER_TOOL_ARGUMENT_MAX_BYTES + 3,
      "Tool argument byte pool must hold one max record plus ring overhead");
  static_assert(
      CONFIG_HAMPTER_TOOL_RESULT_POOL_BYTES >=
          CONFIG_HAMPTER_TOOL_RESULT_MAX_BYTES + 256 + 3,
      "Tool result byte pool must hold one max result plus wire overhead");
  static_assert(
      CONFIG_HAMPTER_FRAME_PAYLOAD_BYTES >=
          CONFIG_HAMPTER_TOOL_ARGUMENT_MAX_BYTES + 256,
      "ObjectLink frame must hold one max Tool argument plus dispatch fields");
  static_assert(
      CONFIG_HAMPTER_FRAME_PAYLOAD_BYTES >=
          CONFIG_HAMPTER_TOOL_RESULT_MAX_BYTES + 256,
      "ObjectLink frame must hold one max Tool result plus result fields");
  static constexpr size_t kToolWireMax =
      CONFIG_HAMPTER_TOOL_RESULT_MAX_BYTES + 256;
  static constexpr size_t kToolResultNestingLimit = 8;
  // ArduinoJson's default allocator grows on the heap. Tool code instead gets
  // one resettable arena, so even a malformed result cannot grow past this
  // Runtime-owned allocation budget before encoded-size validation runs.
  static constexpr size_t kToolResultArenaBytes =
      CONFIG_HAMPTER_TOOL_RESULT_MAX_BYTES * 4 + 1024;
  static_assert(kToolResultArenaBytes >= 1536,
                "Tool result arena must fit the bounded failure envelope");
  static constexpr uint32_t kConnectTimeoutMs = 8000;
  static constexpr uint32_t kFrameTimeoutMs = 3000;
  static constexpr uint32_t kWifiTimeoutMs = 20000;
  static constexpr uint32_t kHeartbeatDefaultMs = 15000;
  static constexpr uint32_t kHeartbeatTimeoutMs = 45000;
  static constexpr uint32_t kPortFlushMs = 20;

  class ToolResultArena final : public Allocator {
   public:
    void* allocate(size_t size) override;
    void deallocate(void* pointer) override;
    void* reallocate(void* pointer, size_t newSize) override;
    void reset() { used_ = 0; }

   private:
    struct BlockHeader {
      size_t size;
    };

    alignas(max_align_t) uint8_t storage_[kToolResultArenaBytes]{};
    size_t used_ = 0;
  };

  enum class LinkMode : uint8_t { None, Enrollment, Runtime };
  enum class LinkState : uint8_t {
    Stopped,
    Backoff,
    Resolving,
    Connecting,
    AwaitServerHello,
    AwaitEnrollment,
    AwaitAuthentication,
    AwaitRegistration,
    Active,
    Fatal,
  };
  enum class IoFlow : uint8_t {
    Starting,
    Provisioning,
    JoiningForEnrollment,
    JoiningForRuntime,
    Linking,
    Error,
  };

  struct PortSlot {
    std::atomic<uint32_t> outboundBits{0};
    std::atomic<uint32_t> outboundRevision{0};
    std::atomic<uint32_t> inboundBits{0};
    std::atomic<bool> inboundPending{false};
    uint32_t sentRevision = 0;
    uint64_t outboundSequence = 0;
    uint64_t inboundSequence = 0;
    uint16_t wireHandle = 0;
    bool outbound = false;
  };

  struct ToolJob {
    uint64_t callId = 0;
    uint64_t deadlineMs = 0;
    uint32_t streamId = 0;
    uint32_t epoch = 0;
    uint32_t localDeadlineMs = 0;
    uint16_t toolIndex = 0;
    uint16_t argumentLength = 0;
  };

  struct ToolResultRecord {
    uint64_t callId = 0;
    uint32_t streamId = 0;
    uint32_t epoch = 0;
    uint16_t payloadLength = 0;
  };

  struct InFlightCall {
    bool used = false;
    uint64_t callId = 0;
    uint64_t deadlineMs = 0;
    uint32_t streamId = 0;
    uint32_t epoch = 0;
  };

  struct CachedResult {
    bool valid = false;
    uint64_t callId = 0;
    uint16_t length = 0;
    uint8_t payload[kToolWireMax]{};
  };

  static void ioTaskEntry(void* context);
  static void networkEventHandler(void* context, esp_event_base_t eventBase,
                                  int32_t eventId, void* eventData);
  void ioTask();
  bool buildManifest();
  void appHandleTools();
  void resetAppResultDocument(uint64_t callId);
  void appHandlePorts();
  void appHandleResetButton();

  bool initializeIo();
  bool configurePowerManagement();
  bool initializeWakeFd();
  bool ensureNetworkEventHandlers();
  void removeNetworkEventHandlers();
  void wakeIo();
  void wakeApp();
  void waitForIo(uint32_t maximumMs);
  uint32_t millisecondsUntilNextIoWork();
  bool outboundPortsDirty() const;
  bool appWorkPending() const;
  bool acquireProvisioningPowerLock();
  void releaseProvisioningPowerLock();
  bool startProvisioning();
  static bool provisioningRadioReady(void* context);
  static void provisioningWorkReady(void* context);
  void processProvisioning();
  bool startRuntimeWifi();
  void processRuntimeWifi();
  bool applyWifiTxPower(const char* phase);
  bool applyWifiPowerSave(const char* phase);
  void eraseEnrollment();

  void startLink(LinkMode mode);
  void linkLoop();
  void attemptConnection();
  void connectTls(const IPAddress* resolvedAddress);
  void disconnectAndBackoff(const char* reason, bool fatal = false);
  void resetSession();
  void receiveFrames();
  bool processFrame(const objectlink::Header& header, const uint8_t* payload,
                    size_t length);
  bool processServerHello(const objectlink::Header& header,
                          JsonDocument& document);
  bool processEnrollAck(const objectlink::Header& header,
                        JsonDocument& document);
  bool processAuthAck(const objectlink::Header& header,
                      JsonDocument& document);
  bool processRegisterAck(const objectlink::Header& header,
                          JsonDocument& document);
  bool processPortBatch(JsonDocument& document);
  bool processToolDispatch(const objectlink::Header& header,
                           JsonDocument& document);

  bool sendEnrollment();
  bool sendAuthentication();
  bool sendRegistration();
  bool sendHeartbeat();
  bool sendPortBatch();
  bool sendToolAck(uint32_t streamId, uint64_t callId, const char* disposition,
                   uint32_t retryAfterMs = 0);
  bool sendToolFailure(uint32_t streamId, uint64_t callId,
                       const char* outcome, const char* code,
                       const char* message, bool retryable = false);
  bool sendFrame(objectlink::MessageType type, uint16_t flags,
                 uint32_t streamId, const uint8_t* payload, size_t length);
  bool sendDocument(objectlink::MessageType type, uint16_t flags,
                    uint32_t streamId, JsonDocument& document);
  void flushToolResults();
  void expireToolCalls();

  InFlightCall* findInFlight(uint64_t callId);
  InFlightCall* allocateInFlight(const ToolJob& job);
  void releaseInFlight(uint64_t callId);
  int findToolByHandle(uint16_t handle) const;
  int findPortByHandle(uint16_t handle) const;
  uint32_t nextStreamId();
  uint64_t estimatedServerTimeMs() const;
  void setError(const char* reason, bool fatal = false);
  void publishIdentitySnapshots();

  HampterDeviceConfig config_;
  ToolRegistry& tools_;
  PortRegistry& ports_;
  mutable portMUX_TYPE errorMux_ = portMUX_INITIALIZER_UNLOCKED;
  char lastError_[129]{};
  mutable char lastErrorSnapshot_[129]{};
  mutable portMUX_TYPE identityMux_ = portMUX_INITIALIZER_UNLOCKED;
  portMUX_TYPE portMux_ = portMUX_INITIALIZER_UNLOCKED;
  char deviceIdSnapshot_[97]{};
  char objectIdSnapshot_[97]{};

  std::atomic<HampterDeviceState> state_{HampterDeviceState::Starting};
  std::atomic<bool> stopping_{false};
  std::atomic<bool> resetRequested_{false};
  std::atomic<bool> fatalRequested_{false};
  std::atomic<uint32_t> sessionEpoch_{1};
  IoFlow ioFlow_ = IoFlow::Starting;

  CredentialStore store_;
  ProvisioningPortal portal_;
  SetupIdentity identity_;
  StoredCredentials credentials_;
  ProvisioningRequest pendingProvisioning_;
  uint32_t wifiDeadline_ = 0;
  uint32_t wifiRetryAt_ = 0;

  WiFiClientSecure client_;
  LinkMode linkMode_ = LinkMode::None;
  LinkState linkState_ = LinkState::Stopped;
  HubBootstrap bootstrap_;
  uint32_t reconnectAt_ = 0;
  uint8_t reconnectExponent_ = 0;
  uint8_t enrollmentAttempts_ = 0;
  uint32_t linkStateStartedAt_ = 0;
  uint32_t lastReceivedAt_ = 0;
  uint32_t lastHeartbeatAt_ = 0;
  uint32_t lastPortFlushAt_ = 0;
  uint32_t heartbeatIntervalMs_ = kHeartbeatDefaultMs;
  uint32_t lastReceivedStreamId_ = 0;
  uint32_t streamCounter_ = 0;
  uint32_t handshakeStreamId_ = 0;
  uint64_t serverTimeBaseMs_ = 0;
  uint64_t serverTimeBaseLocalMs_ = 0;
  uint64_t lastHeartbeatMonotonicMs_ = 0;
  uint64_t lastHeartbeatAckedMonotonicMs_ = 0;
  bool heartbeatOutstanding_ = false;
  bool enrollmentCommitted_ = false;

  uint8_t rxHeader_[objectlink::kHeaderSize]{};
  uint8_t frameBuffer_[objectlink::kHeaderSize +
                       CONFIG_HAMPTER_FRAME_PAYLOAD_BYTES]{};
  uint8_t* framePayload() { return frameBuffer_ + objectlink::kHeaderSize; }
  const uint8_t* framePayload() const {
    return frameBuffer_ + objectlink::kHeaderSize;
  }
  size_t rxHeaderOffset_ = 0;
  size_t rxPayloadOffset_ = 0;
  objectlink::Header rxDecodedHeader_{};
  uint32_t rxFrameStartedAt_ = 0;
  uint32_t negotiatedMaxPayload_ = CONFIG_HAMPTER_FRAME_PAYLOAD_BYTES;
  JsonDocument manifestDocument_;
  JsonDocument ioDocument_;

  PortSlot portSlots_[CONFIG_HAMPTER_MAX_PORTS]{};
  uint16_t toolHandles_[CONFIG_HAMPTER_MAX_TOOLS]{};
  InFlightCall inFlight_[CONFIG_HAMPTER_TOOL_DESCRIPTOR_DEPTH]{};
  CachedResult cachedResult_;

  SpscRing<ToolJob, CONFIG_HAMPTER_TOOL_DESCRIPTOR_DEPTH> toolJobs_;
  SpscByteRing<CONFIG_HAMPTER_TOOL_BYTE_POOL_BYTES> toolArguments_;
  SpscRing<ToolResultRecord, CONFIG_HAMPTER_TOOL_DESCRIPTOR_DEPTH>
      toolResults_;
  SpscByteRing<CONFIG_HAMPTER_TOOL_RESULT_POOL_BYTES> toolResultBytes_;
  uint8_t appArgumentScratch_[CONFIG_HAMPTER_TOOL_ARGUMENT_MAX_BYTES]{};
  uint8_t appResultScratch_[kToolWireMax]{};
  JsonDocument appArgumentsDocument_;
  ToolResultArena appResultArena_;
  JsonDocument appResultDocument_;
  ObservationBuilder observation_;

  uint32_t resetPressedAt_ = 0;
  bool resetHandled_ = false;

  StaticTask_t ioTaskControl_{};
  alignas(StackType_t)
      uint8_t ioTaskStack_[CONFIG_HAMPTER_IO_TASK_STACK_BYTES]{};
  std::atomic<TaskHandle_t> ioTaskHandle_{nullptr};
  std::atomic<bool> ioTaskCreated_{false};
  StaticSemaphore_t ioTaskDoneControl_{};
  SemaphoreHandle_t ioTaskDone_ = nullptr;
  StaticSemaphore_t appWakeControl_{};
  SemaphoreHandle_t appWake_ = nullptr;
  std::atomic<int> wakeFd_{-1};
  esp_event_handler_instance_t wifiEventHandler_ = nullptr;
  esp_event_handler_instance_t ipEventHandler_ = nullptr;
  bool networkEventHandlersRegistered_ = false;
  esp_pm_lock_handle_t provisioningPowerLock_ = nullptr;
  bool provisioningPowerLockHeld_ = false;
};

}  // namespace hampter::internal
