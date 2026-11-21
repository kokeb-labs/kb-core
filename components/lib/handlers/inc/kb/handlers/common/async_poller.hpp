/**
 * @file async_poller.hpp
 * @brief kb::handlers::AsyncPoller<Op, Bytes, Use>: one asynchronous bus transfer driven by an
 * owner task's service pass, so a hot-path read never waits on the bus (ADR-0016).
 *
 * The poller owns the transfer's buffers, so they outlive the transfer by construction: a pass
 * starts a transfer and returns; each later pass calls `Step(now)` once, which polls without
 * waiting and reports the completion once; the destructor and `Cancel()` end a transfer in flight.
 * Each transfer carries a deadline (`budget` from its start); a completion after it is `Timeout`.
 *
 * @code
 * // In an owner-pass access: start when idle, collect when done, never wait.
 * if (!poller.InFlight()) {
 *   static_cast<void>(poller.StartWriteRead(0x48U, reg, 2U, now, kb::Microseconds{800U}));
 * }
 * const kb::iface::AsyncPoll p = poller.Step(now);
 * if (p.Completion().IsOk()) { publish(decode(poller.Received())); }
 * @endcode
 *
 * `Use::NonBlocking` (the default) refuses at compile time a port whose Start() waits
 * (`AsyncFromSync`); `Use::MayBlock` accepts it for paths that tolerate the wait.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_HANDLERS_COMMON_ASYNC_POLLER_HPP
#define KB_HANDLERS_COMMON_ASYNC_POLLER_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

#include "kb/handlers/common/health.hpp"
#include "kb/interfaces/async.hpp"
#include "kb/utils/status.hpp"
#include "kb/utils/time.hpp"

namespace kb::handlers {

/** @brief Whether a poller accepts a port whose Start() waits for the transfer. */
enum class AsyncUse : uint8_t {
  NonBlocking = 0, ///< Hot or safety-adjacent path: only native asynchronous ports compile.
  MayBlock = 1,    ///< A path that tolerates the wait: AsyncFromSync is accepted.
};

/** @brief The counters of one poller. */
struct AsyncStats {
  uint32_t started{0U};        ///< Transfers started.
  uint32_t refused{0U};        ///< Starts the port or the poller refused.
  uint32_t ok{0U};             ///< Transfers completed with Ok.
  uint32_t failed{0U};         ///< Transfers completed with a failure other than Timeout.
  uint32_t timeouts{0U};       ///< Transfers completed with Timeout (deadline or late).
  uint32_t cancels{0U};        ///< Transfers ended by Cancel() or the destructor.
  uint32_t max_latency_us{0U}; ///< Longest start-to-completion time seen by Step() (saturating).
};

/**
 * @brief One asynchronous transfer at a time over `Op`, with its buffers.
 * @tparam Op An I2cAsync or SpiAsync port.
 * @tparam Bytes The largest transfer (each direction), 1 to 256.
 * @tparam Use NonBlocking (default) or MayBlock.
 */
template <kb::iface::AsyncOperation Op, std::size_t Bytes, AsyncUse Use = AsyncUse::NonBlocking>
class AsyncPoller {
  static_assert((Bytes >= 1U) && (Bytes <= 256U), "Bytes: 1 to 256");
  static_assert((Use == AsyncUse::MayBlock) || kb::iface::NonBlocking<Op>,
                "this port's Start() waits for the transfer (kNonBlocking is false, e.g. "
                "AsyncFromSync): a hot path needs a native asynchronous port, or say "
                "AsyncUse::MayBlock (ADR-0016)");

public:
  /** @brief Over `op`; no I/O. */
  constexpr explicit AsyncPoller(Op& op) noexcept : op_(op) {}
  AsyncPoller(const AsyncPoller&) = delete;
  AsyncPoller& operator=(const AsyncPoller&) = delete;
  AsyncPoller(AsyncPoller&&) = delete;
  AsyncPoller& operator=(AsyncPoller&&) = delete;
  /** @brief Ends a transfer in flight: its buffers die with the poller. */
  ~AsyncPoller() {
    static_cast<void>(Cancel());
  }

  /**
   * @brief Starts writing `tx`, then reading `rx_len` bytes from `address`, due `budget` after
   * `now`.
   * @return Ok when started; Busy while one is in flight; InvalidArgument when a length is above
   * Bytes; the port's refusal otherwise.
   */
  [[nodiscard]] kb::Status StartWriteRead(uint8_t address, std::span<const uint8_t> tx,
                                          std::size_t rx_len, kb::Instant now,
                                          kb::Microseconds budget) noexcept
    requires kb::iface::I2cAsync<Op>
  {
    kb::Status s = prepare(tx, rx_len);
    if (s == kb::Status::Ok) {
      s = op_.StartWriteRead(address, std::span<const uint8_t>{tx_}.first(tx.size()),
                             std::span<uint8_t>{rx_}.first(rx_len),
                             kb::utils::Deadline::After(now, budget));
    }
    return started(s, rx_len, now);
  }

  /**
   * @brief Starts one full-duplex frame of `tx` (or `len` zeros when `tx` is empty), keeping the
   * `len` bytes read, due `budget` after `now`.
   * @return As StartWriteRead(); InvalidArgument also when `tx` is given and is not `len` long.
   */
  [[nodiscard]] kb::Status StartTransfer(std::span<const uint8_t> tx, std::size_t len,
                                         kb::Instant now, kb::Microseconds budget) noexcept
    requires kb::iface::SpiAsync<Op>
  {
    kb::Status s =
        ((!tx.empty()) && (tx.size() != len)) ? kb::Status::InvalidArgument : prepare(tx, len);
    if (s == kb::Status::Ok) {
      s = op_.StartTransfer(std::span<const uint8_t>{tx_}.first(tx.size()),
                            std::span<uint8_t>{rx_}.first(len),
                            kb::utils::Deadline::After(now, budget));
    }
    return started(s, len, now);
  }

  /**
   * @brief One step at `now`: polls the transfer in flight once, never waits.
   * @return Idle when nothing is in flight; Pending; or Done with the outcome (once).
   */
  [[nodiscard]] kb::iface::AsyncPoll Step(kb::Instant now) noexcept {
    kb::iface::AsyncPoll p = kb::iface::AsyncPoll::InState(kb::iface::AsyncState::Idle);
    if (in_flight_) {
      p = op_.Poll(now);
      if (p.state != kb::iface::AsyncState::Pending) {
        in_flight_ = false;
        record(p, now);
      }
    }
    return p;
  }

  /** @brief Ends the transfer in flight (bounded by the port); Ok when nothing is in flight. */
  [[nodiscard]] kb::Status Cancel() noexcept {
    kb::Status s = kb::Status::Ok;
    if (in_flight_) {
      s = op_.Cancel();
      in_flight_ = false;
      stats_.cancels = SaturatingIncrement(stats_.cancels);
    }
    return s;
  }

  /** @brief True while a transfer is in flight. */
  [[nodiscard]] bool InFlight() const noexcept {
    return in_flight_;
  }

  /** @brief The bytes of the last transfer that completed with Ok (empty before one). */
  [[nodiscard]] std::span<const uint8_t> Received() const noexcept {
    return std::span<const uint8_t>{rx_}.first(received_);
  }

  /** @brief A copy of the counters. */
  [[nodiscard]] AsyncStats Stats() const noexcept {
    return stats_;
  }

private:
  [[nodiscard]] kb::Status prepare(std::span<const uint8_t> tx, std::size_t rx_len) noexcept {
    kb::Status s = kb::Status::Ok;
    if (in_flight_) {
      s = kb::Status::Busy;
    } else if ((tx.size() > Bytes) || (rx_len > Bytes)) {
      s = kb::Status::InvalidArgument;
    } else {
      for (std::size_t i = 0U; i < tx.size(); ++i) {
        tx_[i] = tx[i];
      }
    }
    return s;
  }

  [[nodiscard]] kb::Status started(kb::Status s, std::size_t rx_len, kb::Instant now) noexcept {
    if (s == kb::Status::Ok) {
      in_flight_ = true;
      received_ = 0U; // the buffer now belongs to the new transfer
      pending_len_ = rx_len;
      started_at_ = now;
      stats_.started = SaturatingIncrement(stats_.started);
    } else {
      stats_.refused = SaturatingIncrement(stats_.refused);
    }
    return s;
  }

  void record(const kb::iface::AsyncPoll& p, kb::Instant now) noexcept {
    constexpr uint64_t kMax = std::numeric_limits<uint32_t>::max();
    const uint64_t latency = kb::utils::ElapsedUs(now, started_at_);
    const auto clipped = static_cast<uint32_t>((latency > kMax) ? kMax : latency);
    stats_.max_latency_us = (clipped > stats_.max_latency_us) ? clipped : stats_.max_latency_us;
    if (p.status == kb::Status::Ok) {
      stats_.ok = SaturatingIncrement(stats_.ok);
      received_ = pending_len_;
    } else if (p.status == kb::Status::Timeout) {
      stats_.timeouts = SaturatingIncrement(stats_.timeouts);
    } else {
      stats_.failed = SaturatingIncrement(stats_.failed);
    }
  }

  Op& op_;
  std::array<uint8_t, Bytes> tx_{};
  std::array<uint8_t, Bytes> rx_{};
  AsyncStats stats_{};
  kb::Instant started_at_;
  std::size_t pending_len_{0U};
  std::size_t received_{0U};
  bool in_flight_{false};
};

} // namespace kb::handlers

#endif // KB_HANDLERS_COMMON_ASYNC_POLLER_HPP
