/**
 * @file seqlock.hpp
 * @brief kb::handlers::Seqlock<T>: one writer publishes a value, any number of readers on other
 * tasks copy it without a lock (ADR-0009: `Latest()` and `Health()`).
 *
 * The value is kept as an array of `std::atomic<uint32_t>` words, so a reader that races the writer
 * reads torn words, never undefined data, and the even/odd sequence tells it to try again. A read
 * is bounded: after `kReadAttempts` torn copies it gives `Busy` instead of spinning (a reader of
 * higher priority than a preempted writer would otherwise spin for ever).
 *
 * Exactly one context writes at a time: the owner task (model O) or the holder of the handler
 * mutex (model G), which also gives the happens-before between successive writers.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_HANDLERS_COMMON_SEQLOCK_HPP
#define KB_HANDLERS_COMMON_SEQLOCK_HPP

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "kb/utils/result.hpp"
#include "kb/utils/status.hpp"

namespace kb::handlers {

/** @brief What a Seqlock may carry: a trivially copyable value with a default. */
template <typename T>
concept SnapshotValue = std::is_trivially_copyable_v<T> && std::is_default_constructible_v<T> &&
                        std::is_nothrow_copy_constructible_v<T>;

/** @brief The bytes of a T, as a Seqlock packs them into words. */
template <typename T>
using ObjectBytes = std::array<std::byte, sizeof(T)>;

/** @brief Torn copies a reader accepts before it gives up with `Busy`. */
inline constexpr uint32_t kReadAttempts = 4U;

/**
 * @brief A single-writer, multi-reader lock-free cell of T.
 * @tparam T The value (SnapshotValue).
 */
template <SnapshotValue T>
class Seqlock {
public:
  constexpr Seqlock() noexcept = default;
  Seqlock(const Seqlock&) = delete;
  Seqlock& operator=(const Seqlock&) = delete;
  Seqlock(Seqlock&&) = delete;
  Seqlock& operator=(Seqlock&&) = delete;
  ~Seqlock() = default;

  /** @brief Publishes `value`. Only the one writing context may call this. */
  void Publish(const T& value) noexcept {
    const auto bytes = std::bit_cast<ObjectBytes<T>>(value);
    const uint32_t begin = seq_.load(std::memory_order_relaxed) + 1U;
    seq_.store(begin, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    for (std::size_t w = 0U; w < kWords; ++w) {
      words_[w].store(pack(bytes, w), std::memory_order_relaxed);
    }
    seq_.store(begin + 1U, std::memory_order_release);
  }

  /**
   * @brief One read attempt: copies the value and its publish count when no write overlapped it.
   * @return False when a write was in progress or overlapped (try again).
   */
  [[nodiscard]] bool TryRead(T& out, uint32_t& publishes) const noexcept {
    const uint32_t before = seq_.load(std::memory_order_acquire);
    ObjectBytes<T> bytes{};
    for (std::size_t w = 0U; w < kWords; ++w) {
      unpack(words_[w].load(std::memory_order_relaxed), w, bytes);
    }
    std::atomic_thread_fence(std::memory_order_acquire);
    const uint32_t after = seq_.load(std::memory_order_relaxed);
    const bool clean = ((before & 1U) == 0U) && (before == after);
    if (clean) {
      out = std::bit_cast<T>(bytes);
      publishes = before / 2U;
    }
    return clean;
  }

  /** @brief The value, after at most kReadAttempts attempts; `Busy` when every copy was torn. */
  [[nodiscard]] kb::Result<T> Read() const noexcept {
    T value{};
    uint32_t publishes = 0U;
    bool clean = false;
    for (uint32_t attempt = 0U; attempt < kReadAttempts; ++attempt) {
      if (!clean) {
        clean = TryRead(value, publishes);
      }
    }
    return clean ? kb::Result<T>::Success(value) : kb::Result<T>::Failure(kb::Status::Busy);
  }

  /** @brief How many times the value was published (wraps after 2^31). */
  [[nodiscard]] uint32_t Publishes() const noexcept {
    return seq_.load(std::memory_order_acquire) / 2U;
  }

private:
  static constexpr std::size_t kWordBytes = 4U;
  static constexpr std::size_t kWords = (sizeof(T) + kWordBytes - 1U) / kWordBytes;
  static constexpr uint32_t kByteBits = 8U;

  [[nodiscard]] static uint32_t pack(const ObjectBytes<T>& bytes, std::size_t word) noexcept {
    uint32_t value = 0U;
    for (std::size_t b = 0U; b < kWordBytes; ++b) {
      const std::size_t index = (word * kWordBytes) + b;
      if (index < sizeof(T)) {
        const auto shift = static_cast<uint32_t>(b * kByteBits);
        value |= (std::to_integer<uint32_t>(bytes[index]) << shift);
      }
    }
    return value;
  }

  static void unpack(uint32_t value, std::size_t word, ObjectBytes<T>& bytes) noexcept {
    for (std::size_t b = 0U; b < kWordBytes; ++b) {
      const std::size_t index = (word * kWordBytes) + b;
      if (index < sizeof(T)) {
        const auto shift = static_cast<uint32_t>(b * kByteBits);
        bytes[index] = static_cast<std::byte>((value >> shift) & 0xFFU);
      }
    }
  }

  std::array<std::atomic<uint32_t>, kWords> words_{};
  std::atomic<uint32_t> seq_{0U};
};

/**
 * @brief A copy of a published value with its publish count and age, as `Latest()` returns it.
 * @tparam T The value.
 */
template <typename T>
struct Snapshot {
  T value{};             ///< The value as published.
  uint32_t sequence{0U}; ///< Publishes so far, this one included: grows by one per publish.
  uint64_t age_us{0U};   ///< Microseconds between the publish and the read.
};

/** @brief A value with the instant it was published, the payload a snapshot Seqlock carries. */
template <typename T>
struct Stamped {
  T value{};          ///< The value.
  uint64_t at_us{0U}; ///< kb::rtos::Now() at the publish.
  bool valid{false};  ///< False until the first real publish.
};

} // namespace kb::handlers

#endif // KB_HANDLERS_COMMON_SEQLOCK_HPP
