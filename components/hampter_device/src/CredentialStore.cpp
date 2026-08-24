#include "CredentialStore.h"

#include <ArduinoJson.h>
#include <ESP.h>
#include <Preferences.h>
#include <bootloader_random.h>
#include <esp_system.h>
#include <nvs.h>

#include <memory>

namespace hampter::internal {
namespace {

constexpr uint32_t kEnvelopeMagic = 0x48434632;  // HCF2
constexpr uint8_t kEnvelopeVersion = 1;
constexpr size_t kEnvelopeSize = 17;

uint32_t crc32Update(uint32_t crc, const uint8_t* data, size_t length) {
  for (size_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
  }
  return crc;
}

uint32_t envelopeCrc32(const uint8_t* blob, size_t payloadLength) {
  uint32_t crc = crc32Update(0xFFFFFFFFu, blob, 13);
  crc = crc32Update(crc, blob + kEnvelopeSize, payloadLength);
  return ~crc;
}

void putU32(uint8_t* output, uint32_t value) {
  output[0] = static_cast<uint8_t>(value >> 24);
  output[1] = static_cast<uint8_t>(value >> 16);
  output[2] = static_cast<uint8_t>(value >> 8);
  output[3] = static_cast<uint8_t>(value);
}

uint32_t getU32(const uint8_t* input) {
  return static_cast<uint32_t>(input[0]) << 24 |
         static_cast<uint32_t>(input[1]) << 16 |
         static_cast<uint32_t>(input[2]) << 8 | input[3];
}

String randomBase32(size_t characters) {
  static constexpr char kAlphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
  String output;
  if (!output.reserve(characters)) return String();
  uint32_t random = 0;
  uint8_t remaining = 0;
  for (size_t i = 0; i < characters; ++i) {
    if (remaining == 0) {
      random = esp_random();
      remaining = 6;
    }
    output += kAlphabet[random & 31u];
    random >>= 5;
    --remaining;
  }
  return output;
}

String deviceIdFromEfuse() {
  const uint64_t mac = ESP.getEfuseMac();
  char output[24];
  snprintf(output, sizeof(output), "esp32c3-%04X%08X",
           static_cast<unsigned>((mac >> 32) & 0xFFFFu),
           static_cast<unsigned>(mac & 0xFFFFFFFFu));
  return String(output);
}

bool decodeConfig(const uint8_t* blob, size_t blobLength,
                  StoredCredentials& output) {
  if (blob == nullptr || blobLength < kEnvelopeSize ||
      getU32(blob) != kEnvelopeMagic || blob[4] != kEnvelopeVersion) {
    return false;
  }
  const uint32_t payloadLength = getU32(blob + 9);
  if (payloadLength != blobLength - kEnvelopeSize ||
      envelopeCrc32(blob, payloadLength) != getU32(blob + 13)) {
    return false;
  }
  JsonDocument document;
  if (deserializeMsgPack(document, blob + kEnvelopeSize, payloadLength,
                         DeserializationOption::NestingLimit(4))) {
    return false;
  }
  StoredCredentials candidate;
  candidate.generation = getU32(blob + 5);
  candidate.wifiSsid = document["wifi_ssid"] | "";
  candidate.wifiPassword = document["wifi_password"] | "";
  candidate.hubHost = document["hub_host"] | "";
  candidate.hubPort = document["hub_port"] | kDefaultObjectLinkPort;
  candidate.hubFingerprintSha256 = document["hub_fingerprint"] | "";
  candidate.objectId = document["object_id"] | "";
  candidate.deviceCredential = document["credential"] | "";
  candidate.credentialExpiresAtMs =
      document["credential_expires_at_ms"] | 0ULL;
  if (!candidate.valid()) return false;
  output = candidate;
  return true;
}

bool encodeConfig(const StoredCredentials& input,
                  std::unique_ptr<uint8_t[]>& output, size_t& outputLength) {
  JsonDocument document;
  document["wifi_ssid"] = input.wifiSsid;
  document["wifi_password"] = input.wifiPassword;
  document["hub_host"] = input.hubHost;
  document["hub_port"] = input.hubPort;
  document["hub_fingerprint"] = input.hubFingerprintSha256;
  document["object_id"] = input.objectId;
  document["credential"] = input.deviceCredential;
  document["credential_expires_at_ms"] = input.credentialExpiresAtMs;
  const size_t payloadLength = measureMsgPack(document);
  if (payloadLength == 0 ||
      payloadLength > CredentialStore::kMaxConfigBlob - kEnvelopeSize) {
    return false;
  }
  outputLength = kEnvelopeSize + payloadLength;
  output.reset(new (std::nothrow) uint8_t[outputLength]);
  if (!output) return false;
  putU32(output.get(), kEnvelopeMagic);
  output[4] = kEnvelopeVersion;
  putU32(output.get() + 5, input.generation);
  putU32(output.get() + 9, static_cast<uint32_t>(payloadLength));
  if (serializeMsgPack(document, output.get() + kEnvelopeSize,
                       payloadLength) != payloadLength) {
    return false;
  }
  putU32(output.get() + 13, envelopeCrc32(output.get(), payloadLength));
  return true;
}

}  // namespace

bool validFingerprint(const String& value) {
  size_t digits = 0;
  for (size_t i = 0; i < value.length(); ++i) {
    if (value[i] == ':') continue;
    if (!isxdigit(static_cast<unsigned char>(value[i]))) return false;
    ++digits;
  }
  return digits == 64;
}

bool constantTimeEquals(const String& left, const String& right) {
  const size_t maximum = max(left.length(), right.length());
  size_t difference = left.length() ^ right.length();
  for (size_t i = 0; i < maximum; ++i) {
    const uint8_t a = i < left.length() ? left[i] : 0;
    const uint8_t b = i < right.length() ? right[i] : 0;
    difference |= a ^ b;
  }
  return difference == 0;
}

bool HubBootstrap::valid() const {
  if (host.length() == 0 || host.length() > 253 || port == 0 ||
      enrollmentToken.length() < 32 || enrollmentToken.length() > 128 ||
      !validFingerprint(fingerprintSha256)) {
    return false;
  }
  for (size_t i = 0; i < host.length(); ++i) {
    const char c = host[i];
    if (!(isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' ||
          c == ':' || c == '_')) {
      return false;
    }
  }
  return true;
}

bool CredentialStore::readSlot(const char* key, StoredCredentials& output) {
  nvs_handle_t handle = 0;
  if (nvs_open("hampter_cfg", NVS_READONLY, &handle) != ESP_OK) {
    // A missing namespace is the normal first-boot state, not an error.
    return false;
  }
  size_t length = 0;
  const esp_err_t lengthResult = nvs_get_blob(handle, key, nullptr, &length);
  if (length < kEnvelopeSize || length > kMaxConfigBlob) {
    nvs_close(handle);
    return false;
  }
  std::unique_ptr<uint8_t[]> blob(new (std::nothrow) uint8_t[length]);
  const bool read = lengthResult == ESP_OK && blob &&
                    nvs_get_blob(handle, key, blob.get(), &length) == ESP_OK;
  nvs_close(handle);
  return read && decodeConfig(blob.get(), length, output);
}

bool CredentialStore::writeSlot(const char* key,
                                const StoredCredentials& credentials) {
  std::unique_ptr<uint8_t[]> blob;
  size_t length = 0;
  if (!encodeConfig(credentials, blob, length)) return false;
  Preferences preferences;
  if (!preferences.begin("hampter_cfg", false)) return false;
  const bool written = preferences.putBytes(key, blob.get(), length) == length;
  preferences.end();
  if (!written) return false;
  StoredCredentials verified;
  return readSlot(key, verified) &&
         verified.generation == credentials.generation;
}

bool CredentialStore::load(StoredCredentials& output) {
  StoredCredentials first;
  StoredCredentials second;
  const bool firstValid = readSlot("slot0", first);
  const bool secondValid = readSlot("slot1", second);
  if (!firstValid && !secondValid) return false;
  output = firstValid && (!secondValid || first.generation >= second.generation)
               ? first
               : second;
  return true;
}

bool CredentialStore::save(const StoredCredentials& credentials) {
  StoredCredentials current;
  StoredCredentials next = credentials;
  next.generation = load(current) ? current.generation + 1 : 1;
  if (!next.valid()) return false;
  return writeSlot((next.generation & 1u) ? "slot1" : "slot0", next);
}

bool CredentialStore::clearProvisioning() {
  Preferences preferences;
  if (!preferences.begin("hampter_cfg", false)) return false;
  const bool cleared = preferences.clear();
  preferences.end();
  return cleared;
}

bool CredentialStore::loadOrCreateIdentity(SetupIdentity& output) {
  Preferences preferences;
  if (!preferences.begin("hampter_id", false)) return false;
  SetupIdentity identity;
  identity.deviceId = deviceIdFromEfuse();
  identity.softApSsid =
      "HAMPTER-" + identity.deviceId.substring(identity.deviceId.length() - 6);
  identity.softApPassword = preferences.getString("ap_password", "");
  // Older SDK images stored a second local setup secret. The WPA2 SoftAP
  // password is now the sole local secret, so erase the obsolete NVS value
  // during the normal identity migration.
  if (preferences.isKey("setup_code")) {
    (void)preferences.remove("setup_code");
  }
  const bool needsEntropy = identity.softApPassword.length() != 16;
  bool generated = true;
  if (needsEntropy) bootloader_random_enable();
  if (needsEntropy) {
    identity.softApPassword = randomBase32(16);
    if (preferences.putString("ap_password", identity.softApPassword) == 0) {
      generated = false;
    }
  }
  if (needsEntropy) bootloader_random_disable();
  preferences.end();
  if (!generated || !identity.valid()) return false;
  output = identity;
  return true;
}

bool CredentialStore::clearIdentity() {
  Preferences preferences;
  if (!preferences.begin("hampter_id", false)) return false;
  const bool cleared = preferences.clear();
  preferences.end();
  return cleared;
}

}  // namespace hampter::internal
