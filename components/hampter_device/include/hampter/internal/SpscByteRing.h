#pragma once

#include <stddef.h>
#include <stdint.h>

#include <atomic>

namespace hampter::internal {

// Single-producer/single-consumer variable-size record ring. A two-byte
// internal length prefix allows several small Tool calls to share one pool
// instead of reserving max_payload * queue_depth bytes.
template <size_t Capacity>
class SpscByteRing {
  static_assert(Capacity >= 8, "byte ring is too small");

 public:
  bool canPush(size_t length) const {
    if (length > UINT16_MAX) return false;
    return freeBytes() >= length + kPrefix;
  }

  bool tryPush(const uint8_t* bytes, size_t length) {
    if ((bytes == nullptr && length != 0) || !canPush(length)) return false;
    size_t head = head_.load(std::memory_order_relaxed);
    put(head, static_cast<uint8_t>(length >> 8));
    put(head, static_cast<uint8_t>(length));
    for (size_t i = 0; i < length; ++i) put(head, bytes[i]);
    head_.store(head, std::memory_order_release);
    return true;
  }

  bool tryPop(uint8_t* output, size_t capacity, size_t& length) {
    length = 0;
    size_t tail = tail_.load(std::memory_order_relaxed);
    const size_t head = head_.load(std::memory_order_acquire);
    const size_t used = distance(tail, head);
    if (used < kPrefix) return false;
    const uint16_t recordLength =
        static_cast<uint16_t>(peek(tail)) << 8 | peek(increment(tail));
    if (used < static_cast<size_t>(recordLength) + kPrefix ||
        recordLength > capacity || (output == nullptr && recordLength != 0)) {
      return false;
    }
    tail = increment(increment(tail));
    for (size_t i = 0; i < recordLength; ++i) {
      output[i] = peek(tail);
      tail = increment(tail);
    }
    tail_.store(tail, std::memory_order_release);
    length = recordLength;
    return true;
  }

  size_t freeBytes() const {
    const size_t head = head_.load(std::memory_order_acquire);
    const size_t tail = tail_.load(std::memory_order_acquire);
    return Capacity - 1 - distance(tail, head);
  }

  bool empty() const {
    return head_.load(std::memory_order_acquire) ==
           tail_.load(std::memory_order_acquire);
  }

 private:
  static constexpr size_t kPrefix = 2;
  static constexpr size_t increment(size_t value) {
    return value + 1 == Capacity ? 0 : value + 1;
  }
  static constexpr size_t distance(size_t from, size_t to) {
    return to >= from ? to - from : Capacity - from + to;
  }
  uint8_t peek(size_t position) const { return bytes_[position]; }
  void put(size_t& position, uint8_t value) {
    bytes_[position] = value;
    position = increment(position);
  }

  uint8_t bytes_[Capacity]{};
  std::atomic<size_t> head_{0};
  std::atomic<size_t> tail_{0};
};

}  // namespace hampter::internal
