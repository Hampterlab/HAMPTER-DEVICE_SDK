#pragma once

#include <stddef.h>
#include <stdint.h>

#include <atomic>
#include <type_traits>

namespace hampter::internal {

template <typename T, size_t Capacity>
class SpscRing {
  static_assert(Capacity > 0, "SPSC ring capacity must be non-zero");
  static_assert(std::is_trivially_copyable_v<T>,
                "SPSC entries must not own heap-backed state");

 public:
  bool tryPush(const T& value) {
    const size_t head = head_.load(std::memory_order_relaxed);
    const size_t next = increment(head);
    if (next == tail_.load(std::memory_order_acquire)) return false;
    entries_[head] = value;
    head_.store(next, std::memory_order_release);
    return true;
  }

  bool tryPop(T& value) {
    const size_t tail = tail_.load(std::memory_order_relaxed);
    if (tail == head_.load(std::memory_order_acquire)) return false;
    value = entries_[tail];
    tail_.store(increment(tail), std::memory_order_release);
    return true;
  }

  bool empty() const {
    return head_.load(std::memory_order_acquire) ==
           tail_.load(std::memory_order_acquire);
  }

  bool full() const {
    const size_t head = head_.load(std::memory_order_acquire);
    return increment(head) == tail_.load(std::memory_order_acquire);
  }

  static constexpr size_t usableCapacity() { return Capacity; }

 private:
  static constexpr size_t kSlots = Capacity + 1;
  static constexpr size_t increment(size_t value) {
    return value + 1 == kSlots ? 0 : value + 1;
  }

  T entries_[kSlots]{};
  std::atomic<size_t> head_{0};
  std::atomic<size_t> tail_{0};
};

}  // namespace hampter::internal
