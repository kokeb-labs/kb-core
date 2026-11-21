// clang-format off: the table below must keep one row per line.
/**
 * @file health_journal.hpp
 * @brief kb::handlers::HealthJournal<Sink, N>: the handler health path into a fault journal
 * (kb::utils::FaultJournal), so the evidence of a fault survives the reset it may cause.
 *
 * The supervisor task reads each handler's `Health()` (lock-free) and the liveness verdict, and
 * passes them here. Only edges are written, each one record and one bounded store write:
 *
 * | Seen | Record |
 * | --- | --- |
 * | a handler enters Faulted | `Fault`: its last status and the driver's code, value = failures in a row |
 * | a faulted handler serves again | `Health`: value = the new state |
 * | a liveness watch goes stale | `Stall`: value = the newly stale bits, detail = every stale bit |
 *
 * A record the sink refuses is retried at the next observation (the edge is kept). One context.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
// clang-format on
#ifndef KB_HANDLERS_COMMON_HEALTH_JOURNAL_HPP
#define KB_HANDLERS_COMMON_HEALTH_JOURNAL_HPP

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>

#include "kb/handlers/common/liveness.hpp"
#include "kb/handlers/common/types.hpp"
#include "kb/utils/fault_journal.hpp"
#include "kb/utils/result.hpp"
#include "kb/utils/status.hpp"
#include "kb/utils/time.hpp"

namespace kb::handlers {

/** @brief Where fault records go: a FaultJournal (or anything with its Append()). */
template <typename S>
concept FaultSink = requires(S& sink, const kb::utils::FaultRecord& record) {
  { sink.Append(record) } noexcept -> std::same_as<kb::Result<uint32_t>>;
};

/**
 * @brief Writes the health and liveness edges of up to N handlers to `Sink`.
 * @tparam Sink A FaultSink.
 * @tparam N Handlers observed, 1 to 64.
 */
template <FaultSink Sink, std::size_t N>
  requires((N >= 1U) && (N <= 64U))
class HealthJournal {
public:
  /** @brief Over `sink`; every handler starts as not faulted, no watch as stale. */
  constexpr explicit HealthJournal(Sink& sink) noexcept : sink_(sink) {}

  /**
   * @brief Observes handler `index` (its HAL id `source`) through its `health` read at `now`.
   * @return Records written (0 or 1); InvalidArgument for an index past N; the read's failure (a
   * torn read: nothing changes); the sink's failure (retried next time).
   */
  [[nodiscard]] kb::Result<uint32_t> Observe(std::size_t index, uint16_t source,
                                             const kb::Result<HealthRecord>& health,
                                             kb::Instant now) noexcept {
    kb::Result<uint32_t> result = kb::Result<uint32_t>::Failure(kb::Status::InvalidArgument);
    if (index >= N) {
      // InvalidArgument.
    } else if (!health.IsOk()) {
      result = kb::Result<uint32_t>::Failure(health.Error(), health.Detail());
    } else {
      const HealthRecord& h = health.Value();
      const bool faulted = h.state == HealthState::Faulted;
      kb::utils::FaultRecord r{};
      r.time_us = now.Us();
      r.source = source;
      bool write = false;
      if (faulted && (!faulted_[index])) {
        r.kind = kb::utils::RecordKind::Fault;
        r.status = h.last_status;
        r.detail = h.last_detail;
        r.value = h.consecutive;
        write = true;
      } else if (faulted_[index] && IsServing(h.state)) {
        r.kind = kb::utils::RecordKind::Health;
        r.value = static_cast<uint32_t>(h.state);
        write = true;
      } else {
        // No edge.
      }
      result = write ? written(sink_.Append(r)) : kb::Result<uint32_t>::Success(0U);
      if (result.IsOk() && (faulted || IsServing(h.state))) {
        faulted_[index] = faulted;
      }
    }
    return result;
  }

  /**
   * @brief Observes the liveness verdict at `now`.
   * @return Records written (0 or 1); the sink's failure (retried next time).
   */
  [[nodiscard]] kb::Result<uint32_t> ObserveLiveness(const LivenessVerdict& verdict,
                                                     kb::Instant now) noexcept {
    const uint32_t fresh = verdict.stale & (~stale_);
    kb::Result<uint32_t> result = kb::Result<uint32_t>::Success(0U);
    if (fresh != 0U) {
      kb::utils::FaultRecord r{};
      r.kind = kb::utils::RecordKind::Stall;
      r.time_us = now.Us();
      r.status = kb::Status::Timeout;
      r.value = fresh;
      r.detail = verdict.stale;
      result = written(sink_.Append(r));
    }
    if (result.IsOk()) {
      stale_ = verdict.stale;
    }
    return result;
  }

private:
  [[nodiscard]] static kb::Result<uint32_t> written(const kb::Result<uint32_t>& r) noexcept {
    return r.IsOk() ? kb::Result<uint32_t>::Success(1U)
                    : kb::Result<uint32_t>::Failure(r.Error(), r.Detail());
  }

  Sink& sink_;
  std::array<bool, N> faulted_{};
  uint32_t stale_{0U};
};

} // namespace kb::handlers

#endif // KB_HANDLERS_COMMON_HEALTH_JOURNAL_HPP
