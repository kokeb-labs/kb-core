/**
 * @file timer.hpp
 * @brief kb::rtos::Timer: a software timer that calls a function in the timer context, periodic or
 * one-shot.
 *
 * The callback runs in the RTOS's timer service context (FreeRTOS: the timer task): it must be
 * short and must not block. Created explicitly; started and stopped at will.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_RTOS_TIMER_HPP
#define KB_RTOS_TIMER_HPP

#include <cstdint>

#include "kb/rtos/backend.hpp"
#include "kb/utils/result.hpp"
#include "kb/utils/time.hpp"

namespace kb::rtos {

/** @brief A callback of a Timer: a plain function with a context pointer, never throwing. */
using TimerCallback = detail::EntryFn;

/** @brief A software timer, created explicitly. */
class Timer {
public:
  constexpr Timer() noexcept = default;
  Timer(const Timer&) = delete;
  Timer& operator=(const Timer&) = delete;
  Timer(Timer&&) = delete;
  Timer& operator=(Timer&&) = delete;
  ~Timer() = default;

  /**
   * @brief Creates a stopped timer that calls `callback(context)` every `period` (or once, if not
   * `periodic`).
   * @return InvalidArgument for a null callback or a zero period, AlreadyStarted on a second call.
   */
  [[nodiscard]] kb::Result<void> Create(TimerCallback callback, void* context, Milliseconds period,
                                        bool periodic) noexcept {
    kb::Status status = kb::Status::AlreadyStarted;
    if (created_) {
      status = kb::Status::AlreadyStarted;
    } else if ((callback == nullptr) || (period.Count() == 0U)) {
      status = kb::Status::InvalidArgument;
    } else {
      detail::TimerParams params{};
      params.callback = callback;
      params.context = context;
      params.period_ms = period.Count();
      params.periodic = periodic;
      status = backend::TimerCreate(storage_, params);
      created_ = status == kb::Status::Ok;
    }
    return kb::Result<void>::FromStatus(status);
  }

  /** @brief Starts (or restarts) the timer. */
  [[nodiscard]] bool Start() noexcept {
    bool done = false;
    if (created_) {
      done = backend::TimerStart(storage_);
    }
    return done;
  }

  /** @brief Stops the timer. */
  [[nodiscard]] bool Stop() noexcept {
    bool done = false;
    if (created_) {
      done = backend::TimerStop(storage_);
    }
    return done;
  }

private:
  detail::TimerStorage storage_{};
  bool created_{false};
};

} // namespace kb::rtos

#endif // KB_RTOS_TIMER_HPP
