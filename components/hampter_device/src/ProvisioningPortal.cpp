#include "ProvisioningPortal.h"

#include <ArduinoJson.h>
#include <WiFi.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_wifi.h>
#include <lwip/inet.h>
#include <lwip/sockets.h>
#include <sdkconfig.h>

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <utility>

namespace hampter::internal {
namespace {

constexpr char kTag[] = "hampter_prov";
const IPAddress kPortalIp(192, 168, 4, 1);
constexpr char kPortalOrigin[] = "http://192.168.4.1";
constexpr char kPortalLocation[] = "http://192.168.4.1/";
constexpr uint16_t kProvisioningPort = 80;
constexpr size_t kMaxRequestBody = 1024;
constexpr size_t kPortalNonceBytes = 16;
constexpr size_t kPortalNonceCharacters = kPortalNonceBytes * 2;
// ArduinoJson initially allocates a 1024-byte variant pool on 32-bit targets.
// Four KiB bounds the pool, decoded strings, and allocator bookkeeping for
// the accepted 1024-byte wire request without putting the arena on HTTPD's
// task stack.
constexpr size_t kJsonArenaBytes = 4096;
// Keep the provisioning SoftAP on its current channel long enough for the
// JSON response to leave the HTTP stack. Starting STA too soon can move an
// AP+STA radio to the target AP's channel and strand the final ACK.
constexpr uint32_t kRequestHandoffMs = 3000;
constexpr uint32_t kSuccessGraceMs = 8000;

// The page is deliberately self-contained: captive clients have no Internet
// path yet, and loading third-party code would expose the enrollment token.
// The CSP nonce and CSRF value are injected as separate response chunks.
constexpr char kPortalHtmlStart[] = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#171923">
<title>Set up HAMPTER</title>
<link rel="icon" href="data:,">
<style nonce=")HTML";

constexpr char kPortalHtmlAfterStyleNonce[] = R"HTML(">
:root{color-scheme:dark;--bg:#0f1118;--card:#191d29;--line:#303748;--text:#f4f6fb;--muted:#aeb7c8;--accent:#8fe36b;--bad:#ff8b8b}
*{box-sizing:border-box}body{margin:0;min-height:100vh;background:radial-gradient(circle at top,#273047 0,#0f1118 46%);color:var(--text);font:16px/1.45 system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif}
main{width:min(100% - 28px,560px);margin:0 auto;padding:max(28px,env(safe-area-inset-top)) 0 max(34px,env(safe-area-inset-bottom))}
.brand{display:flex;align-items:center;gap:12px;margin:0 4px 18px}.mark{display:grid;place-items:center;width:44px;height:44px;border-radius:14px;background:var(--accent);color:#10200b;font-weight:900}.brand h1{font-size:1.45rem;margin:0}.brand p{margin:2px 0 0;color:var(--muted);font-size:.92rem}
.card{background:color-mix(in srgb,var(--card) 94%,transparent);border:1px solid var(--line);border-radius:20px;padding:22px;box-shadow:0 20px 55px #0007}
.notice{margin:0 0 18px;padding:12px 14px;border-radius:12px;background:#11151f;border:1px solid var(--line);color:var(--muted);font-size:.92rem}.notice strong{color:var(--text)}
form{display:grid;gap:15px}fieldset{display:grid;gap:13px;margin:0;padding:0;border:0}legend{width:100%;margin:5px 0 0;padding:0 0 8px;border-bottom:1px solid var(--line);font-weight:750}
label{display:grid;gap:6px;color:var(--muted);font-size:.88rem}input{width:100%;min-height:48px;border:1px solid var(--line);border-radius:11px;background:#0e1119;color:var(--text);font:inherit;padding:11px 12px;outline:none}input:focus{border-color:var(--accent);box-shadow:0 0 0 3px #8fe36b22}.pair{display:grid;grid-template-columns:1fr 110px;gap:10px}
button{min-height:50px;margin-top:4px;border:0;border-radius:12px;background:var(--accent);color:#10200b;font:700 1rem system-ui;cursor:pointer}button:disabled{opacity:.48;cursor:wait}
#status{min-height:48px;margin:17px 0 0;padding:12px 14px;border:1px solid var(--line);border-radius:12px;color:var(--muted);white-space:pre-wrap}#status[data-kind=ok]{border-color:#467b3a;color:#bcebad}#status[data-kind=bad]{border-color:#844646;color:var(--bad)}
.fine{margin:16px 2px 0;color:var(--muted);font-size:.78rem}.hidden{display:none}
@media(max-width:420px){.card{padding:18px}.pair{grid-template-columns:1fr 92px}}
</style>
</head>
<body>
<main>
  <div class="brand"><div class="mark">H</div><div><h1>Set up HAMPTER</h1><p>Protected by this device's WPA2 setup network</p></div></div>
  <section class="card">
    <p class="notice" id="sourceNote"><strong>Step 1 of 1.</strong> Enter the Wi-Fi network this device should join, then use the Hub details from its provisioning QR.</p>
    <form id="setup" autocomplete="off">
      <fieldset>
        <legend>Wi-Fi</legend>
        <label>Network name (SSID)<input id="ssid" maxlength="32" required autocapitalize="none" spellcheck="false"></label>
        <label>Password<input id="password" type="password" maxlength="64" autocomplete="off"></label>
      </fieldset>
      <fieldset>
        <legend>HAMPTER Hub</legend>
        <div class="pair">
          <label>Host<input id="hubHost" maxlength="253" required autocapitalize="none" spellcheck="false" placeholder="hampter.local"></label>
          <label>Port<input id="hubPort" type="number" min="1" max="65535" value="7443" required inputmode="numeric"></label>
        </div>
        <label>Enrollment token<input id="enrollmentToken" type="password" minlength="32" maxlength="128" required autocomplete="off" autocapitalize="none" spellcheck="false"></label>
        <label>TLS SHA-256 fingerprint<input id="tlsSha256" minlength="64" maxlength="95" required autocapitalize="none" spellcheck="false"></label>
      </fieldset>
      <p class="notice hidden" id="expiry"></p>
      <button id="submit" type="submit">Connect and enroll</button>
    </form>
    <div id="status" role="status" aria-live="polite">Waiting for setup details.</div>
    <p class="fine">This page sends credentials only to the device at 192.168.4.1. The device verifies the Hub's pinned TLS fingerprint before using the one-time token.</p>
  </section>
</main>
<script nonce=")HTML";

constexpr char kPortalHtmlAfterScriptNonce[] = R"HTML(">
(()=>{
"use strict";
const csrf=")HTML";

constexpr char kPortalHtmlAfterCsrf[] = R"HTML(";
const byId=id=>document.getElementById(id);
const form=byId("setup"),button=byId("submit"),statusBox=byId("status");
const byteLength=value=>window.TextEncoder?new TextEncoder().encode(value).length:unescape(encodeURIComponent(value)).length;
const setStatus=(message,kind="")=>{statusBox.textContent=message;statusBox.dataset.kind=kind};
const fragmentKeys=["v","hubHost","hubPort","tlsSha256","enrollmentToken","expiresAt"];
let fragmentValid=false,expiresAtMs=0;
try{
  if(location.search)throw new Error("Provisioning data must be in the URL fragment.");
  const encoded=location.hash.replace(/^#/,"");
  if(!encoded||/%(?![0-9a-f]{2})/i.test(encoded))throw new Error("Open a fresh provisioning link from the Hub.");
  const params=new URLSearchParams(encoded);
  const entries=[...params.entries()];
  if(entries.length!==fragmentKeys.length||
     entries.some(([key])=>!fragmentKeys.includes(key))||
     fragmentKeys.some(key=>params.getAll(key).length!==1)){
    throw new Error("The Hub provisioning link has missing, duplicate, or unknown fields.");
  }
  if(params.get("v")!=="1")throw new Error("This Hub provisioning version is not supported.");
  const host=params.get("hubHost");
  const portText=params.get("hubPort");
  const fingerprint=params.get("tlsSha256").toLowerCase();
  const token=params.get("enrollmentToken");
  const expiryText=params.get("expiresAt");
  const port=Number(portText);
  expiresAtMs=Number(expiryText);
  if(!host||byteLength(host)>253||!/^[A-Za-z0-9._:-]+$/.test(host)||
     !/^[1-9][0-9]{0,4}$/.test(portText)||!Number.isInteger(port)||port>65535||
     !/^[0-9a-f]{64}$/.test(fingerprint)||
     !/^[A-Za-z0-9_-]{32,128}$/.test(token)||
     !/^[1-9][0-9]*$/.test(expiryText)||!Number.isSafeInteger(expiresAtMs)){
    throw new Error("The Hub provisioning link contains invalid fields.");
  }
  if(expiresAtMs<=Date.now())throw new Error("This enrollment token has expired. Create a new Hub link.");
  byId("hubHost").value=host;
  byId("hubPort").value=String(port);
  byId("enrollmentToken").value=token;
  byId("tlsSha256").value=fingerprint;
  const expiry=byId("expiry");
  expiry.classList.remove("hidden");
  expiry.textContent="Enrollment token expires "+new Date(expiresAtMs).toLocaleString()+".";
  fragmentValid=true;
}catch(error){
  setStatus(error.message||"Open a fresh provisioning link from the Hub.","bad");
}
if(location.search||location.hash)history.replaceState(null,"","/");
button.disabled=!fragmentValid;
async function pollStatus(){
  try{
    const response=await fetch("/api/status",{cache:"no-store"});
    if(!response.ok)return;
    const state=await response.json();
    if(!fragmentValid&&state.status==="waiting")return;
    const kind=state.status==="complete"?"ok":state.status==="error"||state.status==="expired"?"bad":"";
    setStatus(state.message||state.status,kind);
    if(state.status==="complete"){
      byId("password").value="";
      byId("enrollmentToken").value="";
      button.disabled=true;
    }else if(state.status==="error"){
      button.disabled=!fragmentValid;
    }
  }catch(_){}
}
form.addEventListener("submit",async event=>{
  event.preventDefault();
  if(!fragmentValid||expiresAtMs<=Date.now()){
    fragmentValid=false;button.disabled=true;
    setStatus("The Hub provisioning link is missing or expired. Open a fresh link.","bad");return;
  }
  const fingerprint=byId("tlsSha256").value.replace(/:/g,"").trim().toLowerCase();
  const payload={
    ssid:byId("ssid").value,
    password:byId("password").value,
    hub:{
      host:byId("hubHost").value.trim(),
      port:Number(byId("hubPort").value),
      token:byId("enrollmentToken").value.trim(),
      fingerprint
    }
  };
  if(byteLength(payload.ssid)<1||byteLength(payload.ssid)>32||byteLength(payload.password)>64){
    setStatus("Check the Wi-Fi network name and password length.","bad");return;
  }
  if(!/^[0-9a-f]{64}$/.test(fingerprint)){
    setStatus("The Hub TLS fingerprint must contain 64 hexadecimal characters.","bad");return;
  }
  if(byteLength(payload.hub.token)<32||byteLength(payload.hub.token)>128){
    setStatus("The enrollment token is missing or invalid. Scan a fresh Hub QR.","bad");return;
  }
  button.disabled=true;setStatus("Sending setup details to the device...");
  try{
    const response=await fetch("/api/provision",{
      method:"POST",
      cache:"no-store",
      headers:{"Content-Type":"application/json","X-Hampter-CSRF":csrf},
      body:JSON.stringify(payload)
    });
    const result=await response.json().catch(()=>({}));
    if(!response.ok||!result.accepted)throw new Error(result.error||"request_failed");
    setStatus("Setup accepted. Connecting to Wi-Fi...");
    window.setTimeout(pollStatus,700);
  }catch(error){
    button.disabled=false;
    setStatus("Setup was not accepted ("+(error.message||"request_failed")+").","bad");
  }
});
window.setInterval(pollStatus,1500);
pollStatus();
})();
</script>
</body>
</html>)HTML";

void wipeSecret(String& value) {
  for (size_t i = 0; i < value.length(); ++i) value.setCharAt(i, '\0');
  value = "";
}

void wipeMemory(void* value, size_t length) {
  auto* bytes = static_cast<volatile uint8_t*>(value);
  while (bytes != nullptr && length-- > 0) *bytes++ = 0;
}

// ArduinoJson 7 copies decoded strings into its Allocator even when parsing a
// mutable input buffer. This bounded bump arena keeps every such copy inside
// one allocation. JsonDocument is destroyed first; this allocator then wipes
// the complete arena, including blocks that were released or superseded by a
// reallocation, before returning it to the heap.
class ZeroizingJsonAllocator final : public Allocator {
 public:
  explicit ZeroizingJsonAllocator(size_t capacity) : capacity_(capacity) {
    if (capacity_ <= UINT16_MAX) {
      storage_ = static_cast<uint8_t*>(malloc(capacity_));
      if (storage_ != nullptr) wipeMemory(storage_, capacity_);
    }
  }

  ~ZeroizingJsonAllocator() {
    if (storage_ != nullptr) {
      wipeMemory(storage_, capacity_);
      free(storage_);
    }
    storage_ = nullptr;
    capacity_ = 0;
    used_ = 0;
  }

  ZeroizingJsonAllocator(const ZeroizingJsonAllocator&) = delete;
  ZeroizingJsonAllocator& operator=(const ZeroizingJsonAllocator&) = delete;

  bool valid() const { return storage_ != nullptr; }
  bool overflowed() const { return overflowed_; }

  void* allocate(size_t size) override {
    if (storage_ == nullptr || size == 0 || size > UINT16_MAX) {
      overflowed_ = true;
      return nullptr;
    }
    const size_t span = alignedSize(sizeof(BlockHeader) + size);
    if (span < size || used_ > capacity_ || span > capacity_ - used_) {
      overflowed_ = true;
      return nullptr;
    }
    uint8_t* block = storage_ + used_;
    wipeMemory(block, span);
    auto* header = reinterpret_cast<BlockHeader*>(block);
    header->payloadBytes = static_cast<uint16_t>(size);
    header->spanBytes = static_cast<uint16_t>(span);
    header->magic = kBlockMagic;
    header->active = 1;
    used_ += span;
    return block + sizeof(BlockHeader);
  }

  void deallocate(void* pointer) override {
    BlockHeader* header = findHeader(pointer);
    if (header == nullptr) return;
    auto* block = reinterpret_cast<uint8_t*>(header);
    const size_t offset = static_cast<size_t>(block - storage_);
    wipeMemory(static_cast<uint8_t*>(pointer), header->payloadBytes);
    header->active = 0;
    if (offset + header->spanBytes == used_) {
      const size_t span = header->spanBytes;
      wipeMemory(block, span);
      used_ = offset;
    }
  }

  void* reallocate(void* pointer, size_t newSize) override {
    if (pointer == nullptr) return allocate(newSize);
    if (newSize == 0) {
      deallocate(pointer);
      return nullptr;
    }
    BlockHeader* header = findHeader(pointer);
    if (header == nullptr || newSize > UINT16_MAX) {
      overflowed_ = true;
      return nullptr;
    }

    const size_t oldSize = header->payloadBytes;
    auto* block = reinterpret_cast<uint8_t*>(header);
    const size_t offset = static_cast<size_t>(block - storage_);
    const size_t newSpan = alignedSize(sizeof(BlockHeader) + newSize);
    if (newSpan < newSize) {
      overflowed_ = true;
      return nullptr;
    }

    if (newSize <= oldSize) {
      wipeMemory(static_cast<uint8_t*>(pointer) + newSize, oldSize - newSize);
      if (offset + header->spanBytes == used_ && newSpan < header->spanBytes) {
        wipeMemory(block + newSpan, header->spanBytes - newSpan);
        used_ = offset + newSpan;
        header->spanBytes = static_cast<uint16_t>(newSpan);
      }
      header->payloadBytes = static_cast<uint16_t>(newSize);
      return pointer;
    }

    if (offset + header->spanBytes == used_ && offset <= capacity_ &&
        newSpan <= capacity_ - offset) {
      wipeMemory(static_cast<uint8_t*>(pointer) + oldSize, newSize - oldSize);
      if (newSpan > header->spanBytes) {
        wipeMemory(block + header->spanBytes, newSpan - header->spanBytes);
      }
      used_ = offset + newSpan;
      header->payloadBytes = static_cast<uint16_t>(newSize);
      header->spanBytes = static_cast<uint16_t>(newSpan);
      return pointer;
    }

    void* replacement = allocate(newSize);
    if (replacement == nullptr) return nullptr;
    memcpy(replacement, pointer, oldSize);
    deallocate(pointer);
    return replacement;
  }

 private:
  struct alignas(max_align_t) BlockHeader {
    uint16_t payloadBytes;
    uint16_t spanBytes;
    uint16_t magic;
    uint16_t active;
  };

  static constexpr uint16_t kBlockMagic = 0x484a;  // "HJ"
  static constexpr size_t kAlignment = alignof(max_align_t);

  static size_t alignedSize(size_t size) {
    return (size + kAlignment - 1) & ~(kAlignment - 1);
  }

  BlockHeader* findHeader(void* pointer) {
    if (storage_ == nullptr || pointer == nullptr) return nullptr;
    const uintptr_t base = reinterpret_cast<uintptr_t>(storage_);
    const uintptr_t address = reinterpret_cast<uintptr_t>(pointer);
    if (address < base + sizeof(BlockHeader) ||
        address >= base + capacity_) {
      return nullptr;
    }
    auto* header = reinterpret_cast<BlockHeader*>(
        static_cast<uint8_t*>(pointer) - sizeof(BlockHeader));
    const size_t offset =
        static_cast<size_t>(reinterpret_cast<uint8_t*>(header) - storage_);
    if (header->magic != kBlockMagic || header->active != 1 ||
        header->spanBytes < sizeof(BlockHeader) + header->payloadBytes ||
        offset > used_ || header->spanBytes > used_ - offset) {
      return nullptr;
    }
    return header;
  }

  uint8_t* storage_ = nullptr;
  size_t capacity_ = 0;
  size_t used_ = 0;
  bool overflowed_ = false;
};

bool timeReached(uint32_t now, uint32_t deadline) {
  return static_cast<int32_t>(now - deadline) >= 0;
}

constexpr uint8_t kPortalIpv4[] = {192, 168, 4, 1};
constexpr uint8_t kPortalIpv4Mapped[] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 192, 168, 4, 1,
};

constexpr bool isPortalAddressBytes(const uint8_t* address, size_t length) {
  const uint8_t* expected = nullptr;
  if (length == sizeof(kPortalIpv4)) {
    expected = kPortalIpv4;
  } else if (length == sizeof(kPortalIpv4Mapped)) {
    expected = kPortalIpv4Mapped;
  } else {
    return false;
  }
  if (address == nullptr) return false;
  for (size_t i = 0; i < length; ++i) {
    if (address[i] != expected[i]) return false;
  }
  return true;
}

constexpr uint8_t kNonPortalIpv4[] = {192, 168, 4, 2};
constexpr uint8_t kNativeIpv6Loopback[] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1,
};
static_assert(isPortalAddressBytes(kPortalIpv4, sizeof(kPortalIpv4)));
static_assert(
    isPortalAddressBytes(kPortalIpv4Mapped, sizeof(kPortalIpv4Mapped)));
static_assert(
    !isPortalAddressBytes(kNonPortalIpv4, sizeof(kNonPortalIpv4)));
static_assert(
    !isPortalAddressBytes(kNativeIpv6Loopback, sizeof(kNativeIpv6Loopback)));
static_assert(!isPortalAddressBytes(nullptr, 0));

bool isPortalLocalEndpoint(int socketFd) {
  if (socketFd < 0) return false;

  sockaddr_storage local{};
  socklen_t localLength = sizeof(local);
  if (getsockname(socketFd, reinterpret_cast<sockaddr*>(&local),
                  &localLength) != 0) {
    return false;
  }

  if (local.ss_family == AF_INET) {
    if (localLength < sizeof(sockaddr_in)) return false;
    const auto* address = reinterpret_cast<const sockaddr_in*>(&local);
    return address->sin_port == htons(kProvisioningPort) &&
           isPortalAddressBytes(
               reinterpret_cast<const uint8_t*>(&address->sin_addr.s_addr),
               sizeof(address->sin_addr.s_addr));
  }

#if CONFIG_LWIP_IPV6
  if (local.ss_family == AF_INET6) {
    if (localLength < sizeof(sockaddr_in6)) return false;
    const auto* address = reinterpret_cast<const sockaddr_in6*>(&local);
    return address->sin6_port == htons(kProvisioningPort) &&
           isPortalAddressBytes(address->sin6_addr.s6_addr,
                                sizeof(address->sin6_addr.s6_addr));
  }
#endif

  return false;
}

esp_err_t portalSessionOpen(httpd_handle_t, int socketFd) {
  // esp_http_server listens on every netif. Reject a connection before reading
  // any HTTP bytes unless its actual destination was the protected SoftAP.
  // Host, Origin and CSRF checks remain separate browser-layer defenses.
  return isPortalLocalEndpoint(socketFd) ? ESP_OK : ESP_FAIL;
}

class MutexGuard {
 public:
  explicit MutexGuard(SemaphoreHandle_t mutex) : mutex_(mutex) {
    locked_ =
        mutex_ != nullptr && xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE;
  }
  ~MutexGuard() {
    if (locked_) xSemaphoreGive(mutex_);
  }
  bool locked() const { return locked_; }

 private:
  SemaphoreHandle_t mutex_ = nullptr;
  bool locked_ = false;
};

class ProvisioningRequestSecretGuard {
 public:
  explicit ProvisioningRequestSecretGuard(ProvisioningRequest& request)
      : request_(request) {}
  ~ProvisioningRequestSecretGuard() {
    if (!released_) {
      wipeSecret(request_.wifiPassword);
      wipeSecret(request_.hub.enrollmentToken);
    }
  }
  void release() { released_ = true; }

 private:
  ProvisioningRequest& request_;
  bool released_ = false;
};

class InputSecretGuard {
 public:
  InputSecretGuard(uint8_t* input, size_t length)
      : input_(input), length_(length) {}
  ~InputSecretGuard() { wipeMemory(input_, length_); }

 private:
  uint8_t* input_;
  size_t length_;
};

const char* defaultStatusMessage(ProvisioningStatus status) {
  switch (status) {
    case ProvisioningStatus::Waiting:
      return "Enter Wi-Fi and Hub details.";
    case ProvisioningStatus::ConnectingWifi:
      return "Connecting to Wi-Fi...";
    case ProvisioningStatus::Enrolling:
      return "Wi-Fi connected. Verifying and enrolling with Hub...";
    case ProvisioningStatus::Complete:
      return "Enrollment complete. Device is online.";
    case ProvisioningStatus::Error:
      return "Setup failed. Check the details and retry.";
  }
  return "Setup status unavailable.";
}

const char* statusName(ProvisioningStatus status) {
  switch (status) {
    case ProvisioningStatus::Waiting: return "waiting";
    case ProvisioningStatus::ConnectingWifi: return "connecting_wifi";
    case ProvisioningStatus::Enrolling: return "enrolling";
    case ProvisioningStatus::Complete: return "complete";
    case ProvisioningStatus::Error: return "error";
  }
  return "error";
}

void copySafeStatusMessage(char* destination, size_t capacity,
                           const char* source) {
  if (destination == nullptr || capacity == 0) return;
  const char* selected = source != nullptr ? source : "";
  size_t written = 0;
  while (selected[written] != '\0' && written + 1 < capacity) {
    const unsigned char value = static_cast<unsigned char>(selected[written]);
    if (value < 0x20 || value == 0x7f) {
      destination[written] = ' ';
    } else if (value == '"' || value == '\\') {
      destination[written] = value == '"' ? '\'' : '/';
    } else {
      destination[written] = static_cast<char>(value);
    }
    ++written;
  }
  destination[written] = '\0';
}

bool headerValue(httpd_req_t* request, const char* name, char* output,
                 size_t capacity) {
  if (request == nullptr || name == nullptr || output == nullptr ||
      capacity == 0) {
    return false;
  }
  const size_t length = httpd_req_get_hdr_value_len(request, name);
  if (length == 0 || length >= capacity) return false;
  return httpd_req_get_hdr_value_str(request, name, output, capacity) == ESP_OK;
}

bool canonicalHost(httpd_req_t* request) {
  char host[32]{};
  if (!headerValue(request, "Host", host, sizeof(host))) return false;
  return strcmp(host, "192.168.4.1") == 0 ||
         strcmp(host, "192.168.4.1:80") == 0;
}

bool allowedOrigin(httpd_req_t* request) {
  const size_t length = httpd_req_get_hdr_value_len(request, "Origin");
  if (length == 0) return true;
  char origin[40]{};
  if (!headerValue(request, "Origin", origin, sizeof(origin))) return false;
  return strcmp(origin, kPortalOrigin) == 0 ||
         strcmp(origin, "http://192.168.4.1:80") == 0;
}

bool jsonContentType(httpd_req_t* request) {
  char contentType[64]{};
  if (!headerValue(request, "Content-Type", contentType,
                   sizeof(contentType))) {
    return false;
  }
  constexpr char kJson[] = "application/json";
  if (strncasecmp(contentType, kJson, sizeof(kJson) - 1) != 0) return false;
  const char suffix = contentType[sizeof(kJson) - 1];
  return suffix == '\0' || suffix == ';' || suffix == ' ' || suffix == '\t';
}

bool constantTimeTokenEquals(const char* expected, const char* supplied) {
  uint8_t difference = 0;
  for (size_t i = 0; i < kPortalNonceCharacters; ++i) {
    difference |= static_cast<uint8_t>(expected[i]) ^
                  static_cast<uint8_t>(supplied[i]);
  }
  difference |= static_cast<uint8_t>(expected[kPortalNonceCharacters]);
  difference |= static_cast<uint8_t>(supplied[kPortalNonceCharacters]);
  return difference == 0;
}

int hexNibble(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
}

int expectedWireKey(const char* decoded, size_t decodedLength) {
  static constexpr const char* kKeys[] = {
      "ssid", "password", "hub", "host", "port", "token", "fingerprint",
  };
  for (size_t i = 0; i < sizeof(kKeys) / sizeof(kKeys[0]); ++i) {
    const size_t expectedLength = strlen(kKeys[i]);
    if (decodedLength == expectedLength &&
        memcmp(decoded, kKeys[i], expectedLength) == 0) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

// ArduinoJson intentionally keeps the last value for a duplicate key. Scan
// the already syntax-validated wire JSON as well so a request cannot smuggle
// duplicate root or Hub fields through that behavior. JSON Unicode escapes in
// keys are decoded before comparison.
bool exactExpectedWireKeyOccurrences(const char* json, size_t length) {
  uint8_t counts[7]{};
  for (size_t i = 0; i < length;) {
    if (json[i] != '"') {
      ++i;
      continue;
    }
    ++i;
    char decoded[16]{};
    size_t decodedLength = 0;
    bool overflow = false;
    bool closed = false;
    while (i < length) {
      char value = json[i++];
      if (value == '"') {
        closed = true;
        break;
      }
      if (value == '\\') {
        if (i >= length) return false;
        const char escaped = json[i++];
        if (escaped == 'u') {
          if (i + 4 > length) return false;
          uint16_t codePoint = 0;
          for (uint8_t digit = 0; digit < 4; ++digit) {
            const int nibble = hexNibble(json[i++]);
            if (nibble < 0) return false;
            codePoint =
                static_cast<uint16_t>((codePoint << 4) | nibble);
          }
          value = codePoint <= 0x7f ? static_cast<char>(codePoint) : '\xff';
        } else {
          switch (escaped) {
            case '"':
            case '\\':
            case '/': value = escaped; break;
            case 'b': value = '\b'; break;
            case 'f': value = '\f'; break;
            case 'n': value = '\n'; break;
            case 'r': value = '\r'; break;
            case 't': value = '\t'; break;
            default: return false;
          }
        }
      }
      if (decodedLength < sizeof(decoded)) {
        decoded[decodedLength++] = value;
      } else {
        overflow = true;
      }
    }
    if (!closed) return false;
    size_t after = i;
    while (after < length &&
           (json[after] == ' ' || json[after] == '\t' ||
            json[after] == '\r' || json[after] == '\n')) {
      ++after;
    }
    if (after < length && json[after] == ':' && !overflow) {
      const int key = expectedWireKey(decoded, decodedLength);
      if (key >= 0 && counts[key] != UINT8_MAX) ++counts[key];
    }
  }
  for (uint8_t count : counts) {
    if (count != 1) return false;
  }
  return true;
}

bool exactObjectKeys(JsonObjectConst object, const char* const* expected,
                     size_t expectedCount) {
  if (object.isNull() || object.size() != expectedCount) return false;
  for (JsonPairConst pair : object) {
    bool found = false;
    for (size_t i = 0; i < expectedCount; ++i) {
      if (strcmp(pair.key().c_str(), expected[i]) == 0) {
        found = true;
        break;
      }
    }
    if (!found) return false;
  }
  return true;
}

esp_err_t applyCommonHeaders(httpd_req_t* request, const char* csp) {
  esp_err_t result =
      httpd_resp_set_hdr(request, "Cache-Control", "no-store, max-age=0");
  if (result == ESP_OK) {
    result = httpd_resp_set_hdr(request, "Pragma", "no-cache");
  }
  if (result == ESP_OK) {
    result = httpd_resp_set_hdr(request, "Expires", "0");
  }
  if (result == ESP_OK) {
    result =
        httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
  }
  if (result == ESP_OK) {
    result = httpd_resp_set_hdr(request, "Referrer-Policy", "no-referrer");
  }
  if (result == ESP_OK) {
    result = httpd_resp_set_hdr(request, "X-Frame-Options", "DENY");
  }
  if (result == ESP_OK) {
    result = httpd_resp_set_hdr(request, "Cross-Origin-Resource-Policy",
                                "same-origin");
  }
  if (result == ESP_OK) {
    result = httpd_resp_set_hdr(request, "Content-Security-Policy", csp);
  }
  return result;
}

esp_err_t sendJsonResult(httpd_req_t* request, const char* httpStatus,
                         bool accepted, const char* errorCode) {
  if (request == nullptr) return ESP_ERR_INVALID_ARG;
  if (applyCommonHeaders(request, "default-src 'none'; frame-ancestors 'none'") !=
      ESP_OK) {
    return ESP_FAIL;
  }
  httpd_resp_set_type(request, "application/json; charset=utf-8");
  httpd_resp_set_status(request, httpStatus);
  if (accepted) {
    return httpd_resp_send(request, "{\"accepted\":true}",
                           HTTPD_RESP_USE_STRLEN);
  }
  char body[96]{};
  const char* code = errorCode != nullptr ? errorCode : "internal_error";
  const int length =
      snprintf(body, sizeof(body), "{\"accepted\":false,\"error\":\"%s\"}",
               code);
  if (length <= 0 || static_cast<size_t>(length) >= sizeof(body)) {
    return ESP_FAIL;
  }
  return httpd_resp_send(request, body, length);
}

esp_err_t sendRedirect(httpd_req_t* request) {
  if (applyCommonHeaders(request, "default-src 'none'; frame-ancestors 'none'") !=
      ESP_OK) {
    return ESP_FAIL;
  }
  httpd_resp_set_status(request, "302 Found");
  httpd_resp_set_hdr(request, "Location", kPortalLocation);
  httpd_resp_set_type(request, "text/plain; charset=utf-8");
  return httpd_resp_send(request, "Open the HAMPTER setup page.",
                         HTTPD_RESP_USE_STRLEN);
}

}  // namespace

ProvisioningPortal::ProvisioningPortal() {
  requestMutex_ = xSemaphoreCreateMutexStatic(&requestMutexStorage_);
}

ProvisioningPortal::~ProvisioningPortal() {
  stop();
  if (httpServer_ != nullptr) {
    // A failed httpd_stop means its task may still own this object. Let the
    // configured ESP panic policy reset or halt instead of freeing live state.
    ESP_LOGE(kTag, "provisioning HTTP task survived portal destruction");
    abort();
  }
  if (requestMutex_ != nullptr) {
    vSemaphoreDelete(requestMutex_);
    requestMutex_ = nullptr;
  }
}

bool ProvisioningPortal::generatePortalNonce() {
  uint8_t bytes[kPortalNonceBytes]{};
  esp_fill_random(bytes, sizeof(bytes));
  static constexpr char kHex[] = "0123456789abcdef";
  for (size_t i = 0; i < sizeof(bytes); ++i) {
    portalNonce_[i * 2] = kHex[bytes[i] >> 4];
    portalNonce_[i * 2 + 1] = kHex[bytes[i] & 0x0f];
  }
  portalNonce_[kPortalNonceCharacters] = '\0';
  wipeMemory(bytes, sizeof(bytes));
  return portalNonce_[0] != '\0';
}

bool ProvisioningPortal::startWebPortal() {
  if (!stopWebPortal()) return false;
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = kProvisioningPort;
  config.stack_size = CONFIG_HAMPTER_PROVISIONING_HTTPD_STACK_BYTES;
  config.max_uri_handlers = 4;
  config.max_resp_headers = 12;
  config.max_open_sockets = 4;
  config.lru_purge_enable = true;
  config.recv_wait_timeout = 5;
  config.send_wait_timeout = 5;
  config.open_fn = &portalSessionOpen;
  config.uri_match_fn = httpd_uri_match_wildcard;
  esp_err_t result = httpd_start(&httpServer_, &config);
  if (result != ESP_OK) {
    httpServer_ = nullptr;
    ESP_LOGE(kTag, "could not start provisioning HTTP server: %s",
             esp_err_to_name(result));
    return false;
  }

  const httpd_uri_t root = {
      .uri = "/",
      .method = HTTP_GET,
      .handler = &ProvisioningPortal::rootHandler,
      .user_ctx = this,
  };
  const httpd_uri_t status = {
      .uri = "/api/status",
      .method = HTTP_GET,
      .handler = &ProvisioningPortal::statusHandler,
      .user_ctx = this,
  };
  const httpd_uri_t provision = {
      .uri = "/api/provision",
      .method = HTTP_POST,
      .handler = &ProvisioningPortal::provisionHandler,
      .user_ctx = this,
  };
  const httpd_uri_t captive = {
      .uri = "/*",
      .method = HTTP_GET,
      .handler = &ProvisioningPortal::redirectHandler,
      .user_ctx = this,
  };
  result = httpd_register_uri_handler(httpServer_, &root);
  if (result == ESP_OK) {
    result = httpd_register_uri_handler(httpServer_, &status);
  }
  if (result == ESP_OK) {
    result = httpd_register_uri_handler(httpServer_, &provision);
  }
  if (result == ESP_OK) {
    result = httpd_register_uri_handler(httpServer_, &captive);
  }
  if (result != ESP_OK) {
    ESP_LOGE(kTag, "could not register provisioning route: %s",
             esp_err_to_name(result));
    (void)stopWebPortal();
    return false;
  }
  return true;
}

bool ProvisioningPortal::stopWebPortal() {
  if (httpServer_ == nullptr) return true;
  const esp_err_t result = httpd_stop(httpServer_);
  if (result != ESP_OK) {
    ESP_LOGE(kTag, "could not stop provisioning HTTP server: %s",
             esp_err_to_name(result));
    return false;
  }
  httpServer_ = nullptr;
  return true;
}

bool ProvisioningPortal::begin(const SetupIdentity& identity,
                               RadioReadyCallback radioReady,
                               WorkReadyCallback workReady,
                               void* callbackContext) {
  if (!identity.valid() || requestMutex_ == nullptr) return false;
  stop();
  identity_ = identity;
  workReady_ = workReady;
  callbackContext_ = callbackContext;
  WiFi.persistent(false);

  const auto failStart = [this]() {
    captiveDns_.stop();
    if (!stopWebPortal()) abort();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    wipeSecret(identity_.softApPassword);
    wipeMemory(portalNonce_, sizeof(portalNonce_));
    identity_ = SetupIdentity{};
    workReady_ = nullptr;
    callbackContext_ = nullptr;
    active_ = false;
    return false;
  };

  // Configure and verify the board-specific TX profile before any setup
  // beacon is emitted. The per-session CSRF nonce and every HTTP route are
  // ready before the WPA2 AP becomes visible.
  if (!WiFi.mode(WIFI_STA) ||
      (radioReady != nullptr && !radioReady(callbackContext)) ||
      !WiFi.mode(WIFI_AP_STA) || !generatePortalNonce()) {
    return failStart();
  }
  {
    MutexGuard lock(requestMutex_);
    if (!lock.locked()) return failStart();
    status_ = ProvisioningStatus::Waiting;
    copySafeStatusMessage(statusMessage_, sizeof(statusMessage_),
                          defaultStatusMessage(status_));
    successAt_ = 0;
    pendingReadyAt_ = 0;
    hasPending_ = false;
    active_ = true;
  }
  if (!startWebPortal() ||
      !WiFi.softAPConfig(kPortalIp, kPortalIp,
                         IPAddress(255, 255, 255, 0)) ||
      !WiFi.softAP(identity_.softApSsid.c_str(),
                   identity_.softApPassword.c_str(), 6, false, 1)) {
    return failStart();
  }
  wipeSecret(identity_.softApPassword);
  captiveDns_.setTTL(1);
  if (!captiveDns_.start(53, "*", kPortalIp)) return failStart();

  ESP_LOGI(kTag,
           "browser setup ready: SSID=%s URL=http://192.168.4.1/ "
           "max_clients=1 ap_only=1",
           identity_.softApSsid.c_str());
  return true;
}

void ProvisioningPortal::loop() {
  if (!active_) return;
  ProvisioningStatus status;
  uint32_t successAt;
  {
    MutexGuard lock(requestMutex_);
    if (!lock.locked()) return;
    status = status_;
    successAt = successAt_;
  }

  const uint32_t now = millis();
  if (status == ProvisioningStatus::Complete &&
      now - successAt >= kSuccessGraceMs) {
    stop();
  }
}

uint32_t ProvisioningPortal::millisecondsUntilNextWork(
    uint32_t nowMs, uint32_t maximumMs) {
  MutexGuard lock(requestMutex_);
  if (!lock.locked() || !active_) return maximumMs;
  auto reduceDeadline = [&](uint32_t deadline, uint32_t& waitMs) {
    const uint32_t remaining =
        static_cast<int32_t>(nowMs - deadline) >= 0 ? 0 : deadline - nowMs;
    if (remaining < waitMs) waitMs = remaining;
  };

  uint32_t waitMs = maximumMs;
  if (hasPending_ && pendingReadyAt_ != 0) {
    reduceDeadline(pendingReadyAt_, waitMs);
  }
  if (status_ == ProvisioningStatus::Complete && successAt_ != 0) {
    reduceDeadline(successAt_ + kSuccessGraceMs, waitMs);
  }
  return waitMs;
}

void ProvisioningPortal::stop() {
  bool wasActive = false;
  {
    MutexGuard lock(requestMutex_);
    if (lock.locked()) {
      wasActive = active_;
      active_ = false;
    }
  }
  // Stop new DNS discovery, then join the HTTP task before wiping any state a
  // request handler can observe.
  captiveDns_.stop();
  if (!stopWebPortal()) abort();
  if (wasActive) WiFi.softAPdisconnect(true);

  MutexGuard lock(requestMutex_);
  wipeSecret(identity_.softApPassword);
  wipeSecret(pending_.wifiPassword);
  wipeSecret(pending_.hub.enrollmentToken);
  wipeMemory(portalNonce_, sizeof(portalNonce_));
  wipeMemory(statusMessage_, sizeof(statusMessage_));
  identity_ = SetupIdentity{};
  pending_ = ProvisioningRequest{};
  active_ = false;
  hasPending_ = false;
  pendingReadyAt_ = 0;
  workReady_ = nullptr;
  callbackContext_ = nullptr;
}

bool ProvisioningPortal::takeRequest(ProvisioningRequest& output) {
  MutexGuard lock(requestMutex_);
  if (!lock.locked() || !hasPending_ ||
      !timeReached(millis(), pendingReadyAt_)) {
    return false;
  }
  output = std::move(pending_);
  pending_ = ProvisioningRequest{};
  hasPending_ = false;
  pendingReadyAt_ = 0;
  return true;
}

void ProvisioningPortal::setStatus(ProvisioningStatus status,
                                   const char* safeMessage) {
  MutexGuard lock(requestMutex_);
  if (!lock.locked()) return;
  status_ = status;
  copySafeStatusMessage(
      statusMessage_, sizeof(statusMessage_),
      safeMessage != nullptr ? safeMessage : defaultStatusMessage(status));
  if (status == ProvisioningStatus::Complete) successAt_ = millis();
}

esp_err_t ProvisioningPortal::rootHandler(httpd_req_t* request) {
  if (request == nullptr || request->user_ctx == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }
  return static_cast<ProvisioningPortal*>(request->user_ctx)
      ->serveRoot(request);
}

esp_err_t ProvisioningPortal::statusHandler(httpd_req_t* request) {
  if (request == nullptr || request->user_ctx == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }
  return static_cast<ProvisioningPortal*>(request->user_ctx)
      ->serveStatus(request);
}

esp_err_t ProvisioningPortal::provisionHandler(httpd_req_t* request) {
  if (request == nullptr || request->user_ctx == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }
  return static_cast<ProvisioningPortal*>(request->user_ctx)
      ->handleProvision(request);
}

esp_err_t ProvisioningPortal::redirectHandler(httpd_req_t* request) {
  return request != nullptr ? sendRedirect(request) : ESP_ERR_INVALID_ARG;
}

esp_err_t ProvisioningPortal::serveRoot(httpd_req_t* request) {
  if (!canonicalHost(request)) return sendRedirect(request);
  char nonce[sizeof(portalNonce_)]{};
  {
    MutexGuard lock(requestMutex_);
    if (!lock.locked() || !active_) {
      return sendJsonResult(request, "503 Service Unavailable", false,
                            "not_ready");
    }
    memcpy(nonce, portalNonce_, sizeof(nonce));
  }
  InputSecretGuard nonceGuard(reinterpret_cast<uint8_t*>(nonce),
                              sizeof(nonce));
  char csp[320]{};
  const int cspLength = snprintf(
      csp, sizeof(csp),
      "default-src 'none'; script-src 'nonce-%s'; style-src 'nonce-%s'; "
      "connect-src 'self'; img-src data:; form-action 'self'; "
      "frame-ancestors 'none'; base-uri 'none'",
      nonce, nonce);
  if (cspLength <= 0 || static_cast<size_t>(cspLength) >= sizeof(csp) ||
      applyCommonHeaders(request, csp) != ESP_OK) {
    return ESP_FAIL;
  }
  httpd_resp_set_type(request, "text/html; charset=utf-8");
  esp_err_t result =
      httpd_resp_send_chunk(request, kPortalHtmlStart, HTTPD_RESP_USE_STRLEN);
  if (result == ESP_OK) {
    result = httpd_resp_send_chunk(request, nonce, kPortalNonceCharacters);
  }
  if (result == ESP_OK) {
    result = httpd_resp_send_chunk(request, kPortalHtmlAfterStyleNonce,
                                   HTTPD_RESP_USE_STRLEN);
  }
  if (result == ESP_OK) {
    result = httpd_resp_send_chunk(request, nonce, kPortalNonceCharacters);
  }
  if (result == ESP_OK) {
    result = httpd_resp_send_chunk(request, kPortalHtmlAfterScriptNonce,
                                   HTTPD_RESP_USE_STRLEN);
  }
  if (result == ESP_OK) {
    result = httpd_resp_send_chunk(request, nonce, kPortalNonceCharacters);
  }
  if (result == ESP_OK) {
    result = httpd_resp_send_chunk(request, kPortalHtmlAfterCsrf,
                                   HTTPD_RESP_USE_STRLEN);
  }
  if (result == ESP_OK) result = httpd_resp_send_chunk(request, nullptr, 0);
  return result;
}

esp_err_t ProvisioningPortal::serveStatus(httpd_req_t* request) {
  if (!canonicalHost(request)) return sendRedirect(request);
  char message[sizeof(statusMessage_)]{};
  char deviceId[32]{};
  ProvisioningStatus status = ProvisioningStatus::Error;
  {
    MutexGuard lock(requestMutex_);
    if (!lock.locked() || !active_) {
      return sendJsonResult(request, "503 Service Unavailable", false,
                            "not_ready");
    }
    status = status_;
    memcpy(message, statusMessage_, sizeof(message));
    strlcpy(deviceId, identity_.deviceId.c_str(), sizeof(deviceId));
  }
  char body[384]{};
  const int bodyLength = snprintf(
      body, sizeof(body),
      "{\"portal_version\":1,\"device_id\":\"%s\",\"status\":\"%s\","
      "\"message\":\"%s\"}",
      deviceId, statusName(status), message);
  if (bodyLength <= 0 || static_cast<size_t>(bodyLength) >= sizeof(body) ||
      applyCommonHeaders(request,
                         "default-src 'none'; frame-ancestors 'none'") !=
          ESP_OK) {
    return ESP_FAIL;
  }
  httpd_resp_set_type(request, "application/json; charset=utf-8");
  return httpd_resp_send(request, body, bodyLength);
}

esp_err_t ProvisioningPortal::handleProvision(httpd_req_t* request) {
  if (!canonicalHost(request) || !allowedOrigin(request)) {
    return sendJsonResult(request, "403 Forbidden", false, "forbidden");
  }
  char suppliedNonce[sizeof(portalNonce_)]{};
  if (!headerValue(request, "X-Hampter-CSRF", suppliedNonce,
                   sizeof(suppliedNonce))) {
    return sendJsonResult(request, "403 Forbidden", false, "csrf");
  }
  InputSecretGuard nonceGuard(reinterpret_cast<uint8_t*>(suppliedNonce),
                              sizeof(suppliedNonce));
  {
    MutexGuard lock(requestMutex_);
    if (!lock.locked()) return ESP_FAIL;
    if (!active_) {
      return sendJsonResult(request, "503 Service Unavailable", false,
                            "not_ready");
    }
    if (!constantTimeTokenEquals(portalNonce_, suppliedNonce)) {
      return sendJsonResult(request, "403 Forbidden", false, "csrf");
    }
    const bool acceptsConfig = status_ == ProvisioningStatus::Waiting ||
                               status_ == ProvisioningStatus::Error;
    if (hasPending_ || !acceptsConfig) {
      return sendJsonResult(request, "409 Conflict", false, "busy");
    }
  }
  if (!jsonContentType(request)) {
    return sendJsonResult(request, "415 Unsupported Media Type", false,
                          "json_required");
  }
  if (request->content_len == 0 || request->content_len > kMaxRequestBody) {
    return sendJsonResult(request, "413 Payload Too Large", false,
                          "request_size");
  }

  uint8_t input[kMaxRequestBody + 1]{};
  InputSecretGuard inputGuard(input, request->content_len + 1);
  size_t received = 0;
  uint8_t timeouts = 0;
  while (received < request->content_len) {
    const int result = httpd_req_recv(
        request, reinterpret_cast<char*>(input) + received,
        request->content_len - received);
    if (result == HTTPD_SOCK_ERR_TIMEOUT && timeouts++ == 0) continue;
    if (result <= 0) {
      return sendJsonResult(request, "400 Bad Request", false,
                            "request_read");
    }
    received += static_cast<size_t>(result);
  }
  input[received] = '\0';

  ProvisioningRequest candidate;
  ProvisioningRequestSecretGuard candidateGuard(candidate);
  const char* errorCode = nullptr;
  if (!parseRequest(reinterpret_cast<char*>(input), received, candidate,
                    errorCode)) {
    return sendJsonResult(request, "400 Bad Request", false, errorCode);
  }

  WorkReadyCallback workReady = nullptr;
  void* callbackContext = nullptr;
  {
    MutexGuard lock(requestMutex_);
    if (!lock.locked()) return ESP_FAIL;
    const bool acceptsConfig = status_ == ProvisioningStatus::Waiting ||
                               status_ == ProvisioningStatus::Error;
    if (!active_ || hasPending_ || !acceptsConfig) {
      return sendJsonResult(request, active_ ? "409 Conflict"
                                            : "503 Service Unavailable",
                            false, active_ ? "busy" : "not_ready");
    }
    pending_ = std::move(candidate);
    hasPending_ = true;
    pendingReadyAt_ = millis() + kRequestHandoffMs;
    status_ = ProvisioningStatus::ConnectingWifi;
    copySafeStatusMessage(statusMessage_, sizeof(statusMessage_),
                          defaultStatusMessage(status_));
    candidateGuard.release();
    workReady = workReady_;
    callbackContext = callbackContext_;
  }

  const esp_err_t response =
      sendJsonResult(request, "202 Accepted", true, nullptr);
  if (workReady != nullptr) workReady(callbackContext);
  return response;
}

bool ProvisioningPortal::parseRequest(char* body, size_t length,
                                      ProvisioningRequest& output,
                                      const char*& errorCode) {
  if (body == nullptr || length == 0 || length > kMaxRequestBody) {
    errorCode = "request_size";
    return false;
  }
  ZeroizingJsonAllocator jsonAllocator(kJsonArenaBytes);
  if (!jsonAllocator.valid()) {
    errorCode = "parser_memory";
    return false;
  }
  JsonDocument document(&jsonAllocator);
  const DeserializationError parseError =
      deserializeJson(document, body, length,
                      DeserializationOption::NestingLimit(5));
  if (parseError) {
    errorCode = parseError == DeserializationError::NoMemory ||
                        jsonAllocator.overflowed()
                    ? "request_complexity"
                    : "malformed_json";
    return false;
  }
  if (!document.is<JsonObject>()) {
    errorCode = "malformed_json";
    return false;
  }

  static constexpr const char* kRootKeys[] = {"ssid", "password", "hub"};
  static constexpr const char* kHubKeys[] = {
      "host", "port", "token", "fingerprint",
  };
  JsonObjectConst root = document.as<JsonObjectConst>();
  JsonObjectConst hub = root["hub"].as<JsonObjectConst>();
  if (!exactExpectedWireKeyOccurrences(body, length) ||
      !exactObjectKeys(root, kRootKeys,
                       sizeof(kRootKeys) / sizeof(kRootKeys[0])) ||
      !exactObjectKeys(hub, kHubKeys,
                       sizeof(kHubKeys) / sizeof(kHubKeys[0]))) {
    errorCode = "invalid_fields";
    return false;
  }

  JsonVariantConst ssidValue = root["ssid"];
  JsonVariantConst passwordValue = root["password"];
  JsonVariantConst hostValue = hub["host"];
  JsonVariantConst tokenValue = hub["token"];
  JsonVariantConst fingerprintValue = hub["fingerprint"];
  if (!ssidValue.is<const char*>() || !passwordValue.is<const char*>() ||
      !hostValue.is<const char*>() ||
      !tokenValue.is<const char*>() || !fingerprintValue.is<const char*>()) {
    errorCode = "invalid_fields";
    return false;
  }

  ProvisioningRequest candidate;
  ProvisioningRequestSecretGuard secretGuard(candidate);
  candidate.wifiSsid = ssidValue.as<const char*>();
  candidate.wifiPassword = passwordValue.as<const char*>();
  candidate.hub.host = hostValue.as<const char*>();
  JsonVariantConst hubPortValue = hub["port"];
  if (!hubPortValue.is<uint32_t>()) {
    errorCode = "invalid_hub_port";
    return false;
  }
  const uint32_t hubPort = hubPortValue.as<uint32_t>();
  if (hubPort == 0 || hubPort > UINT16_MAX) {
    errorCode = "invalid_hub_port";
    return false;
  }
  candidate.hub.port = static_cast<uint16_t>(hubPort);
  candidate.hub.enrollmentToken = tokenValue.as<const char*>();
  candidate.hub.fingerprintSha256 = fingerprintValue.as<const char*>();
  candidate.hub.fingerprintSha256.replace(":", "");
  candidate.hub.fingerprintSha256.toLowerCase();
  if (candidate.wifiSsid.length() == 0 ||
      candidate.wifiSsid.length() > 32 ||
      candidate.wifiPassword.length() > 64) {
    errorCode = "invalid_wifi_credentials";
    return false;
  }
  if (!candidate.hub.valid()) {
    errorCode = "invalid_hub";
    return false;
  }
  output = std::move(candidate);
  secretGuard.release();
  return true;
}

}  // namespace hampter::internal
