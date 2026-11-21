/**
 * @file ring_buffer.hpp
 * @brief Fixed-capacity FIFOs: RingBuffer<T, N> for one context, SpscRing<T, N> for one producer
 * and one consumer in different contexts (a task and an interrupt). No heap, no lock.
 *
 * - `RingBuffer`: `Push` refuses when full, `PushOverwrite` drops the oldest element; `Pop`,
 * `Peek`, `PeekAt`, `Discard`. Not safe for concurrent use: guard it, or use SpscRing.
 * - `SpscRing`: wait-free `TryPush` (producer only) and `TryPop` (consumer only). Indices are
 * free-running unsigned counters, so `head - tail` is the fill level across a wrap; N is a power of
 * two. Uses only atomic loads and stores of `std::size_t` (acquire and release), which Cortex-M,
 * RISC-V and x86 do natively.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_UTILS_RING_BUFFER_HPP
#define KB_UTILS_RING_BUFFER_HPP

#include <array>
#include <atomic>
#include <cstddef>
#include <type_traits>

#include "kb/utils/static_vector.hpp"

namespace kb::utils {

/**
 * @brief A first-in first-out queue of at most N elements, for use from one context.
 * @tparam T Element type (InlineElement).
 * @tparam N Capacity, at least 1.
 */
template <InlineElement T, std::size_t N>
  requires(N > 0U)
class RingBuffer {
public:
  constexpr RingBuffer() noexcept = default;

  /** @brief The number of queued elements. */
  [[nodiscard]] constexpr std::size_t Size() const noexcept {
    return count_;
  }
  /** @brief The capacity N. */
  [[nodiscard]] static constexpr std::size_t Capacity() noexcept {
    return N;
  }
  /** @brief True when nothing is queued. */
  [[nodiscard]] constexpr bool Empty() const noexcept {
    return count_ == 0U;
  }
  /** @brief True when N elements are queued. */
  [[nodiscard]] constexpr bool Full() const noexcept {
    return count_ == N;
  }

  /** @brief Queues `value` at the back; false (and no change) when full. */
  [[nodiscard]] constexpr bool Push(const T& value) noexcept {
    const bool room = count_ < N;
    if (room) {
      items_[index(count_)] = value;
      ++count_;
    }
    return room;
  }
  /**
   * @brief Queues `value`; when full, drops the oldest element first.
   * @return True when an element was dropped.
   */
  constexpr bool PushOverwrite(const T& value) noexcept {
    const bool dropped = count_ == N;
    if (dropped) {
      items_[head_] = value;
      head_ = next(head_);
    } else {
      items_[index(count_)] = value;
      ++count_;
    }
    return dropped;
  }
  /** @brief Takes the oldest element into `out`; false when empty. */
  [[nodiscard]] constexpr bool Pop(T& out) noexcept {
    const bool any = count_ > 0U;
    if (any) {
      out = items_[head_];
      items_[head_] = T{};
      head_ = next(head_);
      --count_;
    }
    return any;
  }
  /** @brief Copies the oldest element into `out` without removing it; false when empty. */
  [[nodiscard]] constexpr bool Peek(T& out) const noexcept {
    return PeekAt(0U, out);
  }
  /** @brief Copies the element `offset` places after the oldest; false when there is none. */
  [[nodiscard]] constexpr bool PeekAt(std::size_t offset, T& out) const noexcept {
    const bool ok = offset < count_;
    if (ok) {
      out = items_[index(offset)];
    }
    return ok;
  }
  /** @brief Drops up to `count` of the oldest elements; returns how many were dropped. */
  constexpr std::size_t Discard(std::size_t count) noexcept {
    const std::size_t n = (count < count_) ? count : count_;
    for (std::size_t i = 0U; i < n; ++i) {
      items_[head_] = T{};
      head_ = next(head_);
    }
    count_ -= n;
    return n;
  }
  /** @brief Drops every element. */
  constexpr void Clear() noexcept {
    static_cast<void>(Discard(count_));
    head_ = 0U;
  }

private:
  [[nodiscard]] static constexpr std::size_t next(std::size_t i) noexcept {
    return ((i + 1U) == N) ? 0U : (i + 1U);
  }
  [[nodiscard]] constexpr std::size_t index(std::size_t offset) const noexcept {
    // head_ < N and offset < N, so the sum cannot wrap std::size_t.
    return (head_ + offset) % N;
  }

  std::array<T, N> items_{};
  std::size_t head_{0U};
  std::size_t count_{0U};
};

/** @brief True for a non-zero power of two. */
[[nodiscard]] constexpr bool IsPowerOfTwo(std::size_t n) noexcept {
  return (n != 0U) && ((n & (n - 1U)) == 0U);
}

/**
 * @brief A wait-free single-producer single-consumer FIFO of at most N elements.
 *
 * `TryPush` may be called from one context only (for example an interrupt) and `TryPop` from one
 * other context only (for example a task). `Size()` is exact in either of those two contexts and
 * approximate anywhere else.
 *
 * @tparam T Element type (InlineElement).
 * @tparam N Capacity, a power of two.
 */
template <InlineElement T, std::size_t N>
  requires(IsPowerOfTwo(N))
class SpscRing {
public:
  constexpr SpscRing() noexcept = default;

  /** @brief Producer: queues `value`; false when full. */
  [[nodiscard]] bool TryPush(const T& value) noexcept {
    const std::size_t head = head_.load(std::memory_order_relaxed);
    const std::size_t tail = tail_.load(std::memory_order_acquire);
    const bool room = (head - tail) < N;
    if (room) {
      items_[head & kMask] = value;
      head_.store(head + 1U, std::memory_order_release);
    }
    return room;
  }
  /** @brief Consumer: takes the oldest element into `out`; false when empty. */
  [[nodiscard]] bool TryPop(T& out) noexcept {
    const std::size_t tail = tail_.load(std::memory_order_relaxed);
    const std::size_t head = head_.load(std::memory_order_acquire);
    const bool any = head != tail;
    if (any) {
      out = items_[tail & kMask];
      tail_.store(tail + 1U, std::memory_order_release);
    }
    return any;
  }
  /** @brief The fill level (see the class note on exactness). */
  [[nodiscard]] std::size_t Size() const noexcept {
    const std::size_t head = head_.load(std::memory_order_acquire);
    const std::size_t tail = tail_.load(std::memory_order_acquire);
    return head - tail;
  }
  /** @brief The capacity N. */
  [[nodiscard]] static constexpr std::size_t Capacity() noexcept {
    return N;
  }

private:
  static constexpr std::size_t kMask = N - 1U;

  std::array<T, N> items_{};
  std::atomic<std::size_t> head_{0U};
  std::atomic<std::size_t> tail_{0U};
};

} // namespace kb::utils

#endif // KB_UTILS_RING_BUFFER_HPP
