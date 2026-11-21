/**
 * @file health.hpp
 * @brief kb::handlers::HealthTracker: the one place a handler's health record changes, and the
 * lock-free copy other tasks read (ADR-0009, docs/design/handlers.md#lifecycle-and-health).
 *
 * | Event | From | To |
 * | --- | --- | --- |
 * | `BeginStart()` | any | Starting (or Recovering while a recovery runs) |
 * | `StartDone()` | Starting, Recovering | Ready |
 * | `StartFailed()` | Starting, Recovering | Faulted |
 * | `RecordOk()` | Ready, Degraded | Ready |
 * | `RecordFailure()` | Ready, Degraded | Degraded, or Faulted at the threshold or when fatal |
 * | `RecordRefused()` | any | unchanged (a caller's mistake) |
 * | `Stopped()` | any | Unknown |
 *
 * Single-threaded: only the context that owns the handler writes (the owner task, or the holder of
 * the guarded lock). `Read()` may be called from any task.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_HANDLERS_COMMON_HEALTH_HPP
#define KB_HANDLERS_COMMON_HEALTH_HPP

#include <cstdint>
#include <limits>

#include "kb/handlers/common/seqlock.hpp"
#include "kb/handlers/common/types.hpp"
#include "kb/utils/result.hpp"
#include "kb/utils/status.hpp"
#include "kb/utils/time.hpp"

namespace kb::handlers {

/** @brief Adds one to a counter, saturating at its maximum. */
[[nodiscard]] constexpr uint32_t SaturatingIncrement(uint32_t value) noexcept {
  return (value == std::numeric_limits<uint32_t>::max()) ? value : (value + 1U);
}

/** @brief The health record of one handler and its published copy. */
class HealthTracker {
public:
  constexpr HealthTracker() noexcept = default;
  HealthTracker(const HealthTracker&) = delete;
  HealthTracker& operator=(const HealthTracker&) = delete;
  HealthTracker(HealthTracker&&) = delete;
  HealthTracker& operator=(HealthTracker&&) = delete;
  ~HealthTracker() = default;

  /** @brief Sets the policy (thresholds); `fault_after` 0 is taken as 1. */
  constexpr void SetPolicy(const HealthPolicy& policy) noexcept {
    policy_ = policy;
    if (policy_.fault_after == 0U) {
      policy_.fault_after = 1U;
    }
  }

  /** @brief A start-up run begins (`recovering` while Recover() drives it). */
  void BeginStart(bool recovering) noexcept {
    record_.state = recovering ? HealthState::Recovering : HealthState::Starting;
    publish();
  }

  /** @brief The start-up plan completed at `now`. */
  void StartDone(kb::Instant now, bool recovering) noexcept {
    record_.starts = SaturatingIncrement(record_.starts);
    if (recovering) {
      record_.recoveries = SaturatingIncrement(record_.recoveries);
    }
    record_.consecutive = 0U;
    record_.state = HealthState::Ready;
    good(now);
    publish();
  }

  /** @brief The start-up plan failed with `status` and the driver's `detail` code. */
  void StartFailed(kb::Status status, uint32_t detail, kb::Instant now, bool recovering) noexcept {
    record_.start_failures = SaturatingIncrement(record_.start_failures);
    if (recovering) {
      record_.recovery_failures = SaturatingIncrement(record_.recovery_failures);
    }
    record_.consecutive = SaturatingIncrement(record_.consecutive);
    record_.state = HealthState::Faulted;
    bad(status, detail, now);
    publish();
  }

  /** @brief A served call succeeded at `now`. */
  void RecordOk(kb::Instant now) noexcept {
    record_.consecutive = 0U;
    if (IsServing(record_.state)) {
      record_.state = HealthState::Ready;
    }
    good(now);
    publish();
  }

  /**
   * @brief A served call failed with `status` and the driver's `detail` code; `fatal` (the binding
   * says the chip lost its state) faults at once.
   */
  void RecordFailure(kb::Status status, uint32_t detail, bool fatal, kb::Instant now) noexcept {
    record_.consecutive = SaturatingIncrement(record_.consecutive);
    if (IsServing(record_.state)) {
      const bool threshold = record_.consecutive >= policy_.fault_after;
      record_.state = (fatal || threshold) ? HealthState::Faulted : HealthState::Degraded;
    }
    bad(status, detail, now);
    publish();
  }

  /**
   * @brief A served call was refused for the caller's own mistake (a bad argument): recorded as the
   * last error and counted, but the state and the failures in a row do not change.
   */
  void RecordRefused(kb::Status status, uint32_t detail, kb::Instant now) noexcept {
    record_.refused_calls = SaturatingIncrement(record_.refused_calls);
    record_.last_status = status;
    record_.last_detail = detail;
    record_.last_error_us = now.Us();
    record_.has_error = true;
    publish();
  }

  /** @brief Marks the handler Faulted without a call (a reset event, a stuck bus). */
  void Fault(kb::Status status, uint32_t detail, kb::Instant now) noexcept {
    record_.state = HealthState::Faulted;
    bad(status, detail, now);
    publish();
  }

  /** @brief The driver was stopped (Deinitialize): back to Unknown, counters kept. */
  void Stopped() noexcept {
    record_.state = HealthState::Unknown;
    record_.consecutive = 0U;
    publish();
  }

  /** @brief The record as the writing context sees it (no copy race: same context). */
  [[nodiscard]] const HealthRecord& Current() const noexcept {
    return record_;
  }

  /** @brief A lock-free copy for any task; `Busy` only when every bounded attempt was torn. */
  [[nodiscard]] kb::Result<HealthRecord> Read() const noexcept {
    return published_.Read();
  }

private:
  void good(kb::Instant now) noexcept {
    record_.ok_calls = SaturatingIncrement(record_.ok_calls);
    record_.last_good_us = now.Us();
    record_.has_good = true;
  }

  void bad(kb::Status status, uint32_t detail, kb::Instant now) noexcept {
    record_.failed_calls = SaturatingIncrement(record_.failed_calls);
    record_.last_status = status;
    record_.last_detail = detail;
    record_.last_error_us = now.Us();
    record_.has_error = true;
  }

  void publish() noexcept {
    published_.Publish(record_);
  }

  HealthRecord record_{};
  HealthPolicy policy_{};
  Seqlock<HealthRecord> published_;
};

} // namespace kb::handlers

#endif // KB_HANDLERS_COMMON_HEALTH_HPP
