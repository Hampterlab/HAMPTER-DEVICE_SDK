#include "CompactDns.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <string.h>
#include <sys/poll.h>
#include <unistd.h>

#include <esp_timer.h>
#include <lwip/inet.h>
#include <lwip/sockets.h>

namespace hampter::internal {
namespace {

constexpr uint16_t kMdnsPort = 5353;
constexpr uint32_t kMdnsRetryMs = 500;
constexpr size_t kDnsHeaderBytes = 12;
constexpr uint16_t kTypeA = 1;
constexpr uint16_t kClassIn = 1;
constexpr uint16_t kClassMask = 0x7fff;
constexpr uint16_t kClassUnicastResponse = 0x8000;
constexpr uint16_t kFlagResponse = 0x8000;
constexpr size_t kMaxQuestions = 16;
constexpr size_t kMaxRecords = 64;
constexpr size_t kMaxNameSteps = 128;
constexpr size_t kMaxExpandedNameBytes = 254;  // Plus the terminating root.

uint16_t readU16(const uint8_t* bytes) {
  return static_cast<uint16_t>((static_cast<uint16_t>(bytes[0]) << 8) |
                               static_cast<uint16_t>(bytes[1]));
}

void writeU16(uint8_t* bytes, uint16_t value) {
  bytes[0] = static_cast<uint8_t>(value >> 8);
  bytes[1] = static_cast<uint8_t>(value);
}

uint8_t asciiLower(uint8_t value) {
  if (value >= 'A' && value <= 'Z') return value + ('a' - 'A');
  return value;
}

bool wouldBlock() {
  return errno == EAGAIN || errno == EWOULDBLOCK;
}

bool makeNonblocking(int socket) {
  const int flags = fcntl(socket, F_GETFL, 0);
  return flags >= 0 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
}

// Validates an encoded DNS name and returns the first byte following its wire
// representation. Compression targets must point backwards, which is required
// by RFC 1035 and also makes pointer loops impossible.
bool skipName(const uint8_t* packet, size_t length, size_t start,
              size_t& next) {
  if (start >= length) return false;

  size_t position = start;
  size_t expandedBytes = 0;
  bool haveNext = false;

  for (size_t step = 0; step < kMaxNameSteps; ++step) {
    if (position >= length) return false;
    const uint8_t label = packet[position];

    if (label == 0) {
      if (!haveNext) next = position + 1;
      return expandedBytes <= kMaxExpandedNameBytes;
    }

    if ((label & 0xc0) == 0xc0) {
      if (position + 1 >= length) return false;
      const size_t target =
          (static_cast<size_t>(label & 0x3f) << 8) | packet[position + 1];
      if (target >= position || target >= length) return false;
      if (!haveNext) {
        next = position + 2;
        haveNext = true;
      }
      position = target;
      continue;
    }

    if ((label & 0xc0) != 0 || label > 63) return false;
    if (position + 1 + label > length) return false;
    expandedBytes += static_cast<size_t>(label) + 1;
    if (expandedBytes > kMaxExpandedNameBytes) return false;
    position += static_cast<size_t>(label) + 1;
  }
  return false;
}

bool nameEquals(const uint8_t* packet, size_t length, size_t start,
                const char* expected) {
  if (expected == nullptr || start >= length) return false;

  size_t position = start;
  size_t expectedAt = 0;
  size_t expandedBytes = 0;
  bool firstLabel = true;

  for (size_t step = 0; step < kMaxNameSteps; ++step) {
    if (position >= length) return false;
    const uint8_t label = packet[position];

    if (label == 0) {
      return expected[expectedAt] == '\0' ||
             (expected[expectedAt] == '.' && expected[expectedAt + 1] == '\0');
    }

    if ((label & 0xc0) == 0xc0) {
      if (position + 1 >= length) return false;
      const size_t target =
          (static_cast<size_t>(label & 0x3f) << 8) | packet[position + 1];
      if (target >= position || target >= length) return false;
      position = target;
      continue;
    }

    if ((label & 0xc0) != 0 || label == 0 || label > 63 ||
        position + 1 + label > length) {
      return false;
    }
    expandedBytes += static_cast<size_t>(label) + 1;
    if (expandedBytes > kMaxExpandedNameBytes) return false;

    if (!firstLabel) {
      if (expected[expectedAt] != '.') return false;
      ++expectedAt;
    }
    firstLabel = false;
    for (size_t index = 0; index < label; ++index) {
      const uint8_t actual = packet[position + 1 + index];
      const uint8_t wanted = static_cast<uint8_t>(expected[expectedAt]);
      if (wanted == 0 || asciiLower(actual) != asciiLower(wanted)) {
        return false;
      }
      ++expectedAt;
    }
    position += static_cast<size_t>(label) + 1;
  }
  return false;
}

bool normalizedLocalHost(const char* host, size_t& length) {
  if (host == nullptr) return false;
  length = strnlen(host, 254);
  if (length == 0 || length >= 254) return false;
  if (host[length - 1] == '.') --length;
  if (length <= 6) return false;

  constexpr char suffix[] = ".local";
  const size_t suffixAt = length - (sizeof(suffix) - 1);
  for (size_t index = 0; index < sizeof(suffix) - 1; ++index) {
    if (asciiLower(static_cast<uint8_t>(host[suffixAt + index])) !=
        static_cast<uint8_t>(suffix[index])) {
      return false;
    }
  }
  return suffixAt > 0 && host[suffixAt - 1] != '.';
}

size_t makeMdnsQuery(uint8_t* packet, size_t capacity, const char* host,
                     size_t hostLength) {
  if (capacity < kDnsHeaderBytes + 6) return 0;
  memset(packet, 0, kDnsHeaderBytes);
  writeU16(packet + 4, 1);
  size_t outputAt = kDnsHeaderBytes;
  size_t labelAt = 0;

  while (labelAt < hostLength) {
    size_t labelEnd = labelAt;
    while (labelEnd < hostLength && host[labelEnd] != '.') ++labelEnd;
    const size_t labelLength = labelEnd - labelAt;
    if (labelLength == 0 || labelLength > 63 ||
        outputAt + 1 + labelLength + 5 > capacity) {
      return 0;
    }
    packet[outputAt++] = static_cast<uint8_t>(labelLength);
    memcpy(packet + outputAt, host + labelAt, labelLength);
    outputAt += labelLength;
    labelAt = labelEnd + 1;
  }
  packet[outputAt++] = 0;
  writeU16(packet + outputAt, kTypeA);
  outputAt += 2;
  // QU asks responders for a unicast reply, while multicast replies are also
  // accepted after joining 224.0.0.251 below.
  writeU16(packet + outputAt, kClassIn | kClassUnicastResponse);
  outputAt += 2;
  return outputAt;
}

bool parseMdnsAnswer(const uint8_t* packet, size_t length, const char* host,
                     IPAddress& address) {
  if (length < kDnsHeaderBytes) return false;
  const uint16_t flags = readU16(packet + 2);
  if ((flags & kFlagResponse) == 0 || (flags & 0x7800) != 0) return false;

  const uint16_t questions = readU16(packet + 4);
  const uint16_t answers = readU16(packet + 6);
  const uint16_t authorities = readU16(packet + 8);
  const uint16_t additionals = readU16(packet + 10);
  const size_t recordCount = static_cast<size_t>(answers) + authorities +
                             additionals;
  if (questions > kMaxQuestions || recordCount > kMaxRecords) return false;

  size_t position = kDnsHeaderBytes;
  for (uint16_t question = 0; question < questions; ++question) {
    size_t afterName = 0;
    if (!skipName(packet, length, position, afterName) ||
        afterName + 4 > length) {
      return false;
    }
    position = afterName + 4;
  }

  bool found = false;
  uint8_t foundAddress[4] = {};
  for (size_t record = 0; record < recordCount; ++record) {
    const size_t nameAt = position;
    size_t afterName = 0;
    if (!skipName(packet, length, position, afterName) ||
        afterName + 10 > length) {
      return false;
    }
    const uint16_t type = readU16(packet + afterName);
    const uint16_t dnsClass = readU16(packet + afterName + 2);
    const uint32_t ttl =
        (static_cast<uint32_t>(packet[afterName + 4]) << 24) |
        (static_cast<uint32_t>(packet[afterName + 5]) << 16) |
        (static_cast<uint32_t>(packet[afterName + 6]) << 8) |
        static_cast<uint32_t>(packet[afterName + 7]);
    const uint16_t dataLength = readU16(packet + afterName + 8);
    position = afterName + 10;
    if (position + dataLength > length) return false;

    if (type == kTypeA && (dnsClass & kClassMask) == kClassIn && ttl != 0 &&
        dataLength == 4 && nameEquals(packet, length, nameAt, host)) {
      foundAddress[0] = packet[position];
      foundAddress[1] = packet[position + 1];
      foundAddress[2] = packet[position + 2];
      foundAddress[3] = packet[position + 3];
      found = true;
    }
    position += dataLength;
  }
  if (!found || position != length) return false;
  address = IPAddress(foundAddress[0], foundAddress[1], foundAddress[2],
                      foundAddress[3]);
  return true;
}

}  // namespace

CompactDns::~CompactDns() {
  cancelLocalResolve();
}

bool CompactDns::beginLocalResolve(const char* host, uint32_t timeoutMs) {
  cancelLocalResolve();
  size_t hostLength = 0;
  if (timeoutMs == 0 || !normalizedLocalHost(host, hostLength)) return false;
  memcpy(localHost_, host, hostLength);
  localHost_[hostLength] = '\0';
  localHostLength_ = static_cast<uint16_t>(hostLength);

  const size_t queryBytes =
      makeMdnsQuery(packet_, sizeof(packet_), localHost_, localHostLength_);
  if (queryBytes == 0) return false;

  const int socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (socket < 0) return false;
  const int enabled = 1;
  (void)setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, &enabled,
                   sizeof(enabled));
  if (!makeNonblocking(socket)) {
    ::close(socket);
    return false;
  }

  sockaddr_in local = {};
  local.sin_family = AF_INET;
  local.sin_port = htons(kMdnsPort);
  local.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(socket, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) !=
      0) {
    // QU replies may still be received on an ephemeral port if another mDNS
    // owner has already claimed 5353.
    local.sin_port = 0;
    if (bind(socket, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) !=
        0) {
      ::close(socket);
      return false;
    }
  }

  ip_mreq membership = {};
  membership.imr_multiaddr.s_addr = inet_addr("224.0.0.251");
  membership.imr_interface.s_addr = htonl(INADDR_ANY);
  (void)setsockopt(socket, IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership,
                   sizeof(membership));
  const uint8_t ttl = 255;
  (void)setsockopt(socket, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

  // Multicast membership is released automatically when this socket closes.
  localSocket_ = socket;
  const int64_t nowUs = esp_timer_get_time();
  localDeadlineUs_ = nowUs + static_cast<int64_t>(timeoutMs) * 1000;
  localNextSendUs_ = nowUs;
  localResult_ = LocalResolveResult::Pending;
  localActive_ = true;
  return true;
}

LocalResolveResult CompactDns::pollLocalResolve(IPAddress& address) {
  if (!localActive_) {
    if (localResult_ == LocalResolveResult::Resolved) address = localAddress_;
    return localResult_;
  }

  const int64_t nowUs = esp_timer_get_time();
  if (nowUs >= localDeadlineUs_) {
    finishLocalResolve(LocalResolveResult::Failed);
    return localResult_;
  }

  if (nowUs >= localNextSendUs_) {
    const size_t queryBytes = makeMdnsQuery(
        packet_, sizeof(packet_), localHost_, localHostLength_);
    if (queryBytes == 0) {
      finishLocalResolve(LocalResolveResult::Failed);
      return localResult_;
    }
    sockaddr_in destination = {};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(kMdnsPort);
    destination.sin_addr.s_addr = inet_addr("224.0.0.251");
    const ssize_t sent =
        sendto(localSocket_, packet_, queryBytes, MSG_DONTWAIT,
               reinterpret_cast<const sockaddr*>(&destination),
               sizeof(destination));
    if (sent == static_cast<ssize_t>(queryBytes)) {
      localNextSendUs_ =
          nowUs + static_cast<int64_t>(kMdnsRetryMs) * 1000;
    } else if (sent < 0 && wouldBlock()) {
      localNextSendUs_ = nowUs + 20000;
    } else {
      finishLocalResolve(LocalResolveResult::Failed);
      return localResult_;
    }
  }

  pollfd descriptor = {localSocket_, POLLIN, 0};
  const int ready = ::poll(&descriptor, 1, 0);
  if (ready < 0) {
    if (errno == EINTR) return LocalResolveResult::Pending;
    finishLocalResolve(LocalResolveResult::Failed);
    return localResult_;
  }
  if (ready > 0 && (descriptor.revents & (POLLERR | POLLNVAL)) != 0) {
    finishLocalResolve(LocalResolveResult::Failed);
    return localResult_;
  }
  if (ready > 0 && (descriptor.revents & POLLIN) != 0) {
    // A multicast flood is capped per tick so the ObjectLink loop still runs.
    for (size_t datagram = 0; datagram < 8; ++datagram) {
      const ssize_t received =
          recvfrom(localSocket_, packet_, sizeof(packet_), MSG_DONTWAIT,
                   nullptr, nullptr);
      if (received < 0) {
        if (wouldBlock()) break;
        finishLocalResolve(LocalResolveResult::Failed);
        return localResult_;
      }
      if (received > 0 &&
          parseMdnsAnswer(packet_, static_cast<size_t>(received), localHost_,
                          localAddress_)) {
        finishLocalResolve(LocalResolveResult::Resolved);
        address = localAddress_;
        return localResult_;
      }
    }
  }

  if (esp_timer_get_time() >= localDeadlineUs_) {
    finishLocalResolve(LocalResolveResult::Failed);
  }
  return localResult_;
}

void CompactDns::cancelLocalResolve() {
  if (localSocket_ >= 0) {
    ::close(localSocket_);
    localSocket_ = -1;
  }
  localActive_ = false;
  localResult_ = LocalResolveResult::Failed;
  localDeadlineUs_ = 0;
  localNextSendUs_ = 0;
  localHostLength_ = 0;
  localHost_[0] = '\0';
}

void CompactDns::finishLocalResolve(LocalResolveResult result) {
  if (localSocket_ >= 0) {
    ::close(localSocket_);
    localSocket_ = -1;
  }
  localActive_ = false;
  localResult_ = result;
  localDeadlineUs_ = 0;
  localNextSendUs_ = 0;
}

}  // namespace hampter::internal
