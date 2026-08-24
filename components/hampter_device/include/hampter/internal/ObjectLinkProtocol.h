#pragma once

#include <stddef.h>
#include <stdint.h>

namespace hampter::internal::objectlink {

constexpr uint8_t kMagic[4] = {'H', 'A', 'M', 'P'};
constexpr uint8_t kVersion = 2;
constexpr size_t kHeaderSize = 16;

enum class MessageType : uint8_t {
  ServerHello = 1,
  Enroll = 2,
  EnrollAck = 3,
  Authenticate = 4,
  AuthAck = 5,
  Register = 6,
  RegisterAck = 7,
  Heartbeat = 8,
  HeartbeatAck = 9,
  PortBatch = 10,
  ToolInvoke = 11,
  ToolDispatch = 12,
  ToolAck = 13,
  ToolResult = 14,
  Error = 15,
  Goodbye = 16,
};

enum FrameFlags : uint16_t {
  AckRequired = 1u << 0,
  HighPriority = 1u << 1,
};

constexpr uint16_t kKnownFlags = AckRequired | HighPriority;

struct Header {
  MessageType type = MessageType::Error;
  uint16_t flags = 0;
  uint32_t streamId = 0;
  uint32_t payloadLength = 0;
};

enum class HeaderError : uint8_t {
  None,
  BadMagic,
  UnsupportedVersion,
  UnknownType,
  UnknownFlags,
  PayloadTooLarge,
};

inline void writeU16Be(uint8_t* output, uint16_t value) {
  output[0] = static_cast<uint8_t>(value >> 8);
  output[1] = static_cast<uint8_t>(value);
}

inline void writeU32Be(uint8_t* output, uint32_t value) {
  output[0] = static_cast<uint8_t>(value >> 24);
  output[1] = static_cast<uint8_t>(value >> 16);
  output[2] = static_cast<uint8_t>(value >> 8);
  output[3] = static_cast<uint8_t>(value);
}

inline uint16_t readU16Be(const uint8_t* input) {
  return static_cast<uint16_t>(static_cast<uint16_t>(input[0]) << 8) |
         input[1];
}

inline uint32_t readU32Be(const uint8_t* input) {
  return static_cast<uint32_t>(input[0]) << 24 |
         static_cast<uint32_t>(input[1]) << 16 |
         static_cast<uint32_t>(input[2]) << 8 | input[3];
}

inline bool knownType(uint8_t value) {
  return value >= static_cast<uint8_t>(MessageType::ServerHello) &&
         value <= static_cast<uint8_t>(MessageType::Goodbye);
}

inline void encodeHeader(const Header& header, uint8_t output[kHeaderSize]) {
  output[0] = kMagic[0];
  output[1] = kMagic[1];
  output[2] = kMagic[2];
  output[3] = kMagic[3];
  output[4] = kVersion;
  output[5] = static_cast<uint8_t>(header.type);
  writeU16Be(output + 6, header.flags);
  writeU32Be(output + 8, header.streamId);
  writeU32Be(output + 12, header.payloadLength);
}

inline HeaderError decodeHeader(const uint8_t input[kHeaderSize],
                                uint32_t maxPayload, Header& output) {
  if (input[0] != kMagic[0] || input[1] != kMagic[1] ||
      input[2] != kMagic[2] || input[3] != kMagic[3]) {
    return HeaderError::BadMagic;
  }
  if (input[4] != kVersion) return HeaderError::UnsupportedVersion;
  if (!knownType(input[5])) return HeaderError::UnknownType;
  const uint16_t flags = readU16Be(input + 6);
  if ((flags & ~kKnownFlags) != 0) return HeaderError::UnknownFlags;
  const uint32_t payloadLength = readU32Be(input + 12);
  if (payloadLength > maxPayload) return HeaderError::PayloadTooLarge;
  output.type = static_cast<MessageType>(input[5]);
  output.flags = flags;
  output.streamId = readU32Be(input + 8);
  output.payloadLength = payloadLength;
  return HeaderError::None;
}

}  // namespace hampter::internal::objectlink
