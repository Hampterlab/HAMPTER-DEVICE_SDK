#pragma once

#include <Arduino.h>
#include <DNSServer.h>
#include <esp_http_server.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "CompactDns.h"
#include "CredentialStore.h"

namespace hampter::internal {

enum class ProvisioningStatus : uint8_t {
  Waiting,
  ConnectingWifi,
  Enrolling,
  Complete,
  Error,
};

class ProvisioningPortal {
 public:
  using RadioReadyCallback = bool (*)(void* context);
  using WorkReadyCallback = void (*)(void* context);

  ProvisioningPortal();
  ~ProvisioningPortal();

  bool begin(const SetupIdentity& identity, RadioReadyCallback radioReady,
             WorkReadyCallback workReady, void* callbackContext);
  void loop();
  void stop();
  bool active() const { return active_; }
  uint32_t millisecondsUntilNextWork(uint32_t nowMs,
                                     uint32_t maximumMs);
  bool takeRequest(ProvisioningRequest& output);
  void setStatus(ProvisioningStatus status, const char* safeMessage = nullptr);
  bool beginLocalResolve(const char* host, uint32_t timeoutMs) {
    return dns_.beginLocalResolve(host, timeoutMs);
  }
  LocalResolveResult pollLocalResolve(IPAddress& address) {
    return dns_.pollLocalResolve(address);
  }
  void cancelLocalResolve() { dns_.cancelLocalResolve(); }

 private:
  bool startWebPortal();
  bool stopWebPortal();
  bool generatePortalNonce();
  static esp_err_t rootHandler(httpd_req_t* request);
  static esp_err_t statusHandler(httpd_req_t* request);
  static esp_err_t provisionHandler(httpd_req_t* request);
  static esp_err_t redirectHandler(httpd_req_t* request);
  esp_err_t serveRoot(httpd_req_t* request);
  esp_err_t serveStatus(httpd_req_t* request);
  esp_err_t handleProvision(httpd_req_t* request);
  bool parseRequest(char* body, size_t length, ProvisioningRequest& output,
                    const char*& errorCode);

  CompactDns dns_;
  DNSServer captiveDns_;
  httpd_handle_t httpServer_ = nullptr;
  StaticSemaphore_t requestMutexStorage_{};
  SemaphoreHandle_t requestMutex_ = nullptr;
  SetupIdentity identity_;
  ProvisioningRequest pending_;
  char portalNonce_[33]{};
  char statusMessage_[161]{};
  uint32_t successAt_ = 0;
  uint32_t pendingReadyAt_ = 0;
  ProvisioningStatus status_ = ProvisioningStatus::Waiting;
  bool active_ = false;
  bool hasPending_ = false;
  WorkReadyCallback workReady_ = nullptr;
  void* callbackContext_ = nullptr;
};

}  // namespace hampter::internal
