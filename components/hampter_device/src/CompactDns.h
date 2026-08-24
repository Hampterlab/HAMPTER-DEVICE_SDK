#pragma once

#include <IPAddress.h>
#include <stddef.h>
#include <stdint.h>

namespace hampter::internal {

enum class LocalResolveResult : uint8_t {
  Pending,
  Resolved,
  Failed,
};

// A taskless DNS helper. Every method is called by hampter_io; the class owns
// one reusable packet buffer and never creates a worker, queue, or allocation.
class CompactDns {
 public:
  CompactDns() = default;
  ~CompactDns();

  CompactDns(const CompactDns&) = delete;
  CompactDns& operator=(const CompactDns&) = delete;

  // Starts one asynchronous mDNS lookup. pollLocalResolve() performs only
  // nonblocking socket work and must be called by the existing I/O loop.
  bool beginLocalResolve(const char* host, uint32_t timeoutMs);
  LocalResolveResult pollLocalResolve(IPAddress& address);
  void cancelLocalResolve();

 private:
  static constexpr size_t kPacketBytes = 512;

  void finishLocalResolve(LocalResolveResult result);

  int localSocket_ = -1;
  IPAddress localAddress_;
  int64_t localDeadlineUs_ = 0;
  int64_t localNextSendUs_ = 0;
  uint16_t localHostLength_ = 0;
  LocalResolveResult localResult_ = LocalResolveResult::Failed;
  bool localActive_ = false;
  char localHost_[254] = {};
  uint8_t packet_[kPacketBytes] = {};
};

}  // namespace hampter::internal
