/**
 * @file event_flags.hpp
 * @brief kb::rtos::EventFlags: a group of 24 flags that threads wait on, with an ISR-safe set.
 *
 * `WaitAny(bits, timeout)` and `WaitAll(bits, timeout)` return the flags seen when the wait ended;
 * the wait succeeded when `(result & bits)` is non-zero (any) or equals `bits` (all). With `clear`
 * (the default) the waited flags are cleared on success. Only the low 24 bits are flags
 * (kAllFlags), the portable FreeRTOS width.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_RTOS_EVENT_FLAGS_HPP
#define KB_RTOS_EVENT_FLAGS_HPP

#include <cstdint>

#include "kb/rtos/backend.hpp"
#include "kb/rtos/time.hpp"
#include "kb/utils/result.hpp"
#include "kb/utils/time.hpp"

namespace kb::rtos {

/** @brief Every usable flag: the low 24 bits. */
inline constexpr uint32_t kAllFlags = 0x00FFFFFFUL;

/** @brief A group of event flags, created explicitly. */
class EventFlags {
public:
  constexpr EventFlags() noexcept = default;
  EventFlags(const EventFlags&) = delete;
  EventFlags& operator=(const EventFlags&) = delete;
  EventFlags(EventFlags&&) = delete;
  EventFlags& operator=(EventFlags&&) = delete;
  ~EventFlags() = default;

  /** @brief Creates the group, all flags clear. `AlreadyStarted` on a second call. */
  [[nodiscard]] kb::Result<void> Create() noexcept {
    kb::Status status = kb::Status::AlreadyStarted;
    if (!created_) {
      status = backend::EventFlagsCreate(storage_);
      created_ = status == kb::Status::Ok;
    }
    return kb::Result<void>::FromStatus(status);
  }

  /** @brief Sets `bits` (masked to kAllFlags). */
  void Set(uint32_t bits) noexcept {
    if (created_) {
      backend::EventFlagsSet(storage_, bits & kAllFlags);
    }
  }

  /** @brief Sets `bits` from an interrupt handler (ISR-safe). */
  [[nodiscard]] bool SetFromIsr(uint32_t bits) noexcept {
    bool done = false;
    if (created_) {
      done = backend::EventFlagsSetFromIsr(storage_, bits & kAllFlags);
    }
    return done;
  }

  /** @brief Clears `bits`. */
  void Clear(uint32_t bits) noexcept {
    if (created_) {
      backend::EventFlagsClear(storage_, bits & kAllFlags);
    }
  }

  /** @brief The flags now; 0 when not created. */
  [[nodiscard]] uint32_t Get() noexcept {
    uint32_t value = 0U;
    if (created_) {
      value = backend::EventFlagsGet(storage_);
    }
    return value;
  }

  /** @brief Waits at most `timeout` for any of `bits`; returns the flags seen (see the file note).
   */
  [[nodiscard]] uint32_t WaitAny(uint32_t bits, Milliseconds timeout, bool clear = true) noexcept {
    return wait(bits, false, clear, timeout);
  }

  /** @brief Waits at most `timeout` for all of `bits`; returns the flags seen (see the file note).
   */
  [[nodiscard]] uint32_t WaitAll(uint32_t bits, Milliseconds timeout, bool clear = true) noexcept {
    return wait(bits, true, clear, timeout);
  }

private:
  [[nodiscard]] uint32_t wait(uint32_t bits, bool all, bool clear, Milliseconds timeout) noexcept {
    const uint32_t wanted = bits & kAllFlags;
    uint32_t seen = 0U;
    if (created_ && (wanted != 0U)) {
      seen = backend::EventFlagsWait(storage_, wanted, all, clear, ToTicksAtLeast(timeout));
    }
    return seen;
  }

  detail::EventFlagsStorage storage_{};
  bool created_{false};
};

} // namespace kb::rtos

#endif // KB_RTOS_EVENT_FLAGS_HPP
