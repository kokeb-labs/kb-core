/**
 * @file semaphore.hpp
 * @brief kb::rtos::Semaphore: a counting semaphore (a binary one with a maximum of 1), with an
 * ISR-safe give.
 *
 * Created explicitly with `Create(max, initial)`. `TakeFor(timeout)` waits at most `timeout`;
 * `GiveFromIsr()` is the only function here that an interrupt handler may call.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_RTOS_SEMAPHORE_HPP
#define KB_RTOS_SEMAPHORE_HPP

#include <cstdint>

#include "kb/rtos/backend.hpp"
#include "kb/rtos/time.hpp"
#include "kb/utils/result.hpp"
#include "kb/utils/time.hpp"

namespace kb::rtos {

/** @brief A counting semaphore, created explicitly. */
class Semaphore {
public:
  constexpr Semaphore() noexcept = default;
  Semaphore(const Semaphore&) = delete;
  Semaphore& operator=(const Semaphore&) = delete;
  Semaphore(Semaphore&&) = delete;
  Semaphore& operator=(Semaphore&&) = delete;
  /// Deletes the RTOS object (ADR-0017); nothing may hold, wait on or use it any more.
  ~Semaphore() {
    if (created_) {
      backend::SemaphoreDelete(storage_);
    }
  }

  /**
   * @brief Creates the semaphore with `max_count` (at least 1) and `initial` (at most `max_count`).
   * @return InvalidArgument for a bad count, AlreadyStarted on a second call.
   */
  [[nodiscard]] kb::Result<void> Create(uint32_t max_count, uint32_t initial) noexcept {
    kb::Status status = kb::Status::AlreadyStarted;
    if (created_) {
      status = kb::Status::AlreadyStarted;
    } else if ((max_count == 0U) || (initial > max_count)) {
      status = kb::Status::InvalidArgument;
    } else {
      status = backend::SemaphoreCreate(storage_, max_count, initial);
      created_ = status == kb::Status::Ok;
    }
    return kb::Result<void>::FromStatus(status);
  }

  /** @brief Takes one count, waiting at most `timeout`. */
  [[nodiscard]] bool TakeFor(Milliseconds timeout) noexcept {
    bool done = false;
    if (created_) {
      done = backend::SemaphoreTake(storage_, ToTicksAtLeast(timeout));
    }
    return done;
  }

  /** @brief Takes one count if one is available now. */
  [[nodiscard]] bool TryTake() noexcept {
    bool done = false;
    if (created_) {
      done = backend::SemaphoreTake(storage_, 0U);
    }
    return done;
  }

  /** @brief Gives one count; false when not created or already at the maximum. */
  [[nodiscard]] bool Give() noexcept {
    bool done = false;
    if (created_) {
      done = backend::SemaphoreGive(storage_);
    }
    return done;
  }

  /** @brief Gives one count from an interrupt handler (ISR-safe); false when at the maximum. */
  [[nodiscard]] bool GiveFromIsr() noexcept {
    bool done = false;
    if (created_) {
      done = backend::SemaphoreGiveFromIsr(storage_);
    }
    return done;
  }

  /** @brief The current count; 0 when not created. */
  [[nodiscard]] uint32_t Count() noexcept {
    uint32_t value = 0U;
    if (created_) {
      value = backend::SemaphoreCount(storage_);
    }
    return value;
  }

  /** @brief True after a successful Create(). */
  [[nodiscard]] bool IsCreated() const noexcept {
    return created_;
  }

private:
  detail::SemaphoreStorage storage_{};
  bool created_{false};
};

} // namespace kb::rtos

#endif // KB_RTOS_SEMAPHORE_HPP
