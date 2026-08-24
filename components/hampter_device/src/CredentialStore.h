#pragma once

#include <Arduino.h>

namespace hampter::internal {

constexpr uint16_t kDefaultObjectLinkPort = 7443;

struct SetupIdentity {
  String deviceId;
  String softApSsid;
  String softApPassword;

  bool valid() const {
    return deviceId.length() >= 8 && softApSsid.length() > 0 &&
           softApPassword.length() == 16;
  }
};

struct StoredCredentials {
  uint32_t generation = 0;
  String wifiSsid;
  String wifiPassword;
  String hubHost;
  uint16_t hubPort = kDefaultObjectLinkPort;
  String hubFingerprintSha256;
  String objectId;
  String deviceCredential;
  uint64_t credentialExpiresAtMs = 0;

  bool valid() const {
    return wifiSsid.length() > 0 && wifiSsid.length() <= 32 &&
           wifiPassword.length() <= 64 && hubHost.length() > 0 &&
           hubHost.length() <= 253 && hubPort != 0 &&
           hubFingerprintSha256.length() == 64 && objectId.length() > 0 &&
           deviceCredential.length() >= 16;
  }
};

struct HubBootstrap {
  String host;
  uint16_t port = kDefaultObjectLinkPort;
  String enrollmentToken;
  String fingerprintSha256;

  bool valid() const;
};

struct ProvisioningRequest {
  String wifiSsid;
  String wifiPassword;
  HubBootstrap hub;
};

struct EnrollmentResult {
  String objectId;
  String credentialHex;
  String hubFingerprintSha256;
  uint64_t credentialExpiresAtMs = 0;
};

class CredentialStore {
 public:
  static constexpr size_t kMaxConfigBlob = 4096;

  bool load(StoredCredentials& output);
  bool save(const StoredCredentials& credentials);
  bool clearProvisioning();
  bool loadOrCreateIdentity(SetupIdentity& output);
  bool clearIdentity();

 private:
  bool readSlot(const char* key, StoredCredentials& output);
  bool writeSlot(const char* key, const StoredCredentials& credentials);
};

bool validFingerprint(const String& value);
bool constantTimeEquals(const String& left, const String& right);

}  // namespace hampter::internal
