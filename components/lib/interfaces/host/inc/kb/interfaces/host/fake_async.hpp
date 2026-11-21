/**
 * @file fake_async.hpp
 * @brief kb::iface::host::FakeAsyncI2c and FakeAsyncSpi: the asynchronous contract (ADR-0016) over
 * the host I2C and SPI fakes, with fault injection, for host tests. Test code, outside the MISRA
 * claim.
 *
 * A started transfer completes on the n-th Poll() after Start() (`CompleteAfterPolls(n)`, default
 * 1); only then is the synchronous fake run, so the caller's buffers are untouched until the
 * completion is reported. Faults:
 *
 * - `Hang(true)`: never completes; the deadline ends it as `Timeout`.
 * - `CompleteLate(true)`: the completion arrives in the poll at which the deadline has passed (it
 *   must be reported as `Timeout`, not `Ok`).
 * - `CompleteDuringCancel(true)`: the transfer completes inside Cancel() (the cancel race); the
 *   completion is dropped and counted.
 * - NACKs, bus errors and SPI failures come from the wrapped fake (`FailNext`).
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_INTERFACES_HOST_FAKE_ASYNC_HPP
#define KB_INTERFACES_HOST_FAKE_ASYNC_HPP

#include <cstddef>
#include <cstdint>
#include <span>

#include "kb/interfaces/async.hpp"
#include "kb/interfaces/host/fake_i2c.hpp"
#include "kb/interfaces/host/fake_spi.hpp"
#include "kb/utils/status.hpp"
#include "kb/utils/time.hpp"

namespace kb::iface::host {

/// Counters of an asynchronous fake.
struct AsyncCounters {
  uint32_t started{0U};   ///< Start() calls that started a transfer.
  uint32_t refused{0U};   ///< Start() calls refused (Busy, Timeout, InvalidArgument, NotReady).
  uint32_t completed{0U}; ///< Completions reported by Poll() (any outcome).
  uint32_t timeouts{0U};  ///< Completions reported as Timeout (deadline or late).
  uint32_t late{0U};      ///< Completions that arrived after the deadline.
  uint32_t cancels{0U};   ///< Cancel() calls that ended a transfer in flight.
  uint32_t dropped{0U};   ///< Completions that raced a Cancel() and were dropped.
  uint32_t polls{0U};     ///< Poll() calls.
};

/** @brief The scripted engine both fakes share: one transfer in flight, completed by a later
 * Poll(). */
class AsyncScript {
public:
  /** @brief The transfer completes on the n-th Poll() after Start() (n >= 1). */
  void CompleteAfterPolls(uint32_t n) noexcept {
    after_ = (n == 0U) ? 1U : n;
  }
  /** @brief While true, a started transfer never completes. */
  void Hang(bool hang) noexcept {
    hang_ = hang;
  }
  /** @brief While true, completions arrive only once the deadline has passed. */
  void CompleteLate(bool late) noexcept {
    late_ = late;
  }
  /** @brief While true, Cancel() finds the transfer completing (the race). */
  void CompleteDuringCancel(bool race) noexcept {
    race_ = race;
  }
  /** @brief The counters. */
  [[nodiscard]] const AsyncCounters& Counters() const noexcept {
    return counters_;
  }
  /** @brief True while a transfer is in flight. */
  [[nodiscard]] bool InFlight() const noexcept {
    return pending_;
  }

  /// Start(): Busy while in flight, Timeout past the deadline, else in flight.
  [[nodiscard]] kb::Status Begin(kb::Status precheck, kb::utils::Deadline deadline,
                                 kb::Instant now) noexcept {
    kb::Status s = precheck;
    if (pending_) {
      s = kb::Status::Busy;
    } else if ((s == kb::Status::Ok) && deadline.Expired(now)) {
      s = kb::Status::Timeout;
    } else {
      // precheck decides
    }
    if (s == kb::Status::Ok) {
      pending_ = true;
      deadline_ = deadline;
      polls_ = 0U;
      ++counters_.started;
    } else {
      ++counters_.refused;
    }
    return s;
  }

  /// Poll(): runs the transfer through `run` when it completes.
  template <typename RunFn>
  [[nodiscard]] AsyncPoll Step(kb::Instant now, const RunFn& run) noexcept {
    ++counters_.polls;
    AsyncPoll p = AsyncPoll::InState(AsyncState::Idle);
    if (pending_) {
      ++polls_;
      const bool expired = deadline_.Expired(now);
      const bool due = (!hang_) && (polls_ >= after_) && ((!late_) || expired);
      if (due) {
        const RunResult r = run();
        p = AsyncPoll::Completed(expired ? kb::Status::Timeout : r.status, r.count,
                                 static_cast<uint32_t>(r.status));
        if (expired) {
          ++counters_.late;
        }
        finish(p);
      } else if (expired) {
        p = AsyncPoll::Completed(kb::Status::Timeout, 0U);
        finish(p);
      } else {
        p = AsyncPoll::InState(AsyncState::Pending);
      }
    }
    return p;
  }

  /// Cancel(): ends the transfer in flight; a racing completion is run and dropped.
  template <typename RunFn>
  [[nodiscard]] kb::Status Abort(const RunFn& run) noexcept {
    if (pending_) {
      if (race_) {
        static_cast<void>(run());
        ++counters_.dropped;
      }
      pending_ = false;
      ++counters_.cancels;
    }
    return kb::Status::Ok;
  }

  /// What one run of the synchronous fake gave.
  struct RunResult {
    kb::Status status{kb::Status::Ok}; ///< The fake's result.
    std::size_t count{0U};             ///< Bytes moved on success.
  };

private:
  void finish(const AsyncPoll& p) noexcept {
    pending_ = false;
    ++counters_.completed;
    if (p.status == kb::Status::Timeout) {
      ++counters_.timeouts;
    }
  }

  AsyncCounters counters_{};
  kb::utils::Deadline deadline_;
  uint32_t after_{1U};
  uint32_t polls_{0U};
  bool pending_{false};
  bool hang_{false};
  bool late_{false};
  bool race_{false};
};

/** @brief The asynchronous I2C fake over a FakeI2cBus (its devices and its FailNext faults). */
class FakeAsyncI2c {
public:
  static constexpr bool kNonBlocking = true; ///< Start() and Poll() never wait.

  /** @brief Over `bus`; `now` is read by Start() for the deadline check. */
  FakeAsyncI2c(FakeI2cBus& bus, const kb::Instant& now) noexcept : bus_(bus), now_(now) {}

  /** @brief The script (latency and faults). */
  [[nodiscard]] AsyncScript& Script() noexcept {
    return script_;
  }

  /// @name I2cBus, forwarded
  /// @{
  [[nodiscard]] kb::Status EnsureReady() noexcept {
    return bus_.EnsureReady();
  }
  [[nodiscard]] kb::Status Write(uint8_t address, std::span<const uint8_t> tx) noexcept {
    return bus_.Write(address, tx);
  }
  [[nodiscard]] kb::Status WriteRead(uint8_t address, std::span<const uint8_t> tx,
                                     std::span<uint8_t> rx) noexcept {
    return bus_.WriteRead(address, tx, rx);
  }
  [[nodiscard]] kb::Status Read(uint8_t address, std::span<uint8_t> rx) noexcept {
    return bus_.Read(address, rx);
  }
  [[nodiscard]] I2cFailure LastFailure() const noexcept {
    return bus_.LastFailure();
  }
  /// @}

  /// @name I2cAsync
  /// @{
  [[nodiscard]] kb::Status StartWrite(uint8_t address, std::span<const uint8_t> tx,
                                      kb::utils::Deadline deadline) noexcept {
    return begin(Kind::Write, address, tx, {}, deadline);
  }
  [[nodiscard]] kb::Status StartWriteRead(uint8_t address, std::span<const uint8_t> tx,
                                          std::span<uint8_t> rx,
                                          kb::utils::Deadline deadline) noexcept {
    return begin(Kind::WriteRead, address, tx, rx, deadline);
  }
  [[nodiscard]] kb::Status StartRead(uint8_t address, std::span<uint8_t> rx,
                                     kb::utils::Deadline deadline) noexcept {
    return begin(Kind::Read, address, {}, rx, deadline);
  }
  [[nodiscard]] AsyncPoll Poll(kb::Instant now) noexcept {
    return script_.Step(now, [this]() noexcept { return run(); });
  }
  [[nodiscard]] kb::Status Cancel() noexcept {
    return script_.Abort([this]() noexcept { return run(); });
  }
  /// @}

private:
  enum class Kind : uint8_t { Write, WriteRead, Read };

  [[nodiscard]] kb::Status begin(Kind kind, uint8_t address, std::span<const uint8_t> tx,
                                 std::span<uint8_t> rx, kb::utils::Deadline deadline) noexcept {
    const bool valid = (address >= 0x08U) && (address <= 0x77U);
    const kb::Status s =
        script_.Begin(valid ? kb::Status::Ok : kb::Status::InvalidArgument, deadline, now_);
    if (s == kb::Status::Ok) {
      kind_ = kind;
      address_ = address;
      tx_ = tx;
      rx_ = rx;
    }
    return s;
  }

  [[nodiscard]] AsyncScript::RunResult run() noexcept {
    AsyncScript::RunResult r{};
    switch (kind_) {
    case Kind::Write:
      r.status = bus_.Write(address_, tx_);
      r.count = tx_.size();
      break;
    case Kind::WriteRead:
      r.status = bus_.WriteRead(address_, tx_, rx_);
      r.count = rx_.size();
      break;
    case Kind::Read:
      r.status = bus_.Read(address_, rx_);
      r.count = rx_.size();
      break;
    }
    return r;
  }

  FakeI2cBus& bus_;
  const kb::Instant& now_;
  AsyncScript script_{};
  std::span<const uint8_t> tx_;
  std::span<uint8_t> rx_;
  Kind kind_{Kind::Write};
  uint8_t address_{0U};
};

static_assert(kb::iface::I2cAsync<FakeAsyncI2c>, "FakeAsyncI2c conforms to I2cAsync");
static_assert(kb::iface::NonBlocking<FakeAsyncI2c>, "FakeAsyncI2c never waits");

/** @brief The asynchronous SPI fake over a FakeSpiDevice (its scripted answers and faults). */
class FakeAsyncSpi {
public:
  static constexpr bool kNonBlocking = true; ///< Start() and Poll() never wait.

  /** @brief Over `device`; `now` is read by Start() for the deadline check. */
  FakeAsyncSpi(FakeSpiDevice& device, const kb::Instant& now) noexcept
      : device_(device), now_(now) {}

  /** @brief The script (latency and faults). */
  [[nodiscard]] AsyncScript& Script() noexcept {
    return script_;
  }

  /// @name SpiDevice, forwarded
  /// @{
  [[nodiscard]] kb::Status EnsureReady() noexcept {
    return device_.EnsureReady();
  }
  [[nodiscard]] kb::Status Transfer(std::span<const uint8_t> tx, std::span<uint8_t> rx) noexcept {
    return device_.Transfer(tx, rx);
  }
  /// @}

  /// @name SpiAsync
  /// @{
  [[nodiscard]] kb::Status StartTransfer(std::span<const uint8_t> tx, std::span<uint8_t> rx,
                                         kb::utils::Deadline deadline) noexcept {
    const kb::Status s = script_.Begin(
        SpiSpansValid(tx, rx) ? kb::Status::Ok : kb::Status::InvalidArgument, deadline, now_);
    if (s == kb::Status::Ok) {
      tx_ = tx;
      rx_ = rx;
    }
    return s;
  }
  [[nodiscard]] AsyncPoll Poll(kb::Instant now) noexcept {
    return script_.Step(now, [this]() noexcept { return run(); });
  }
  [[nodiscard]] kb::Status Cancel() noexcept {
    return script_.Abort([this]() noexcept { return run(); });
  }
  /// @}

private:
  [[nodiscard]] AsyncScript::RunResult run() noexcept {
    AsyncScript::RunResult r{};
    r.status = device_.Transfer(tx_, rx_);
    r.count = tx_.empty() ? rx_.size() : tx_.size();
    return r;
  }

  FakeSpiDevice& device_;
  const kb::Instant& now_;
  AsyncScript script_{};
  std::span<const uint8_t> tx_;
  std::span<uint8_t> rx_;
};

static_assert(kb::iface::SpiAsync<FakeAsyncSpi>, "FakeAsyncSpi conforms to SpiAsync");

} // namespace kb::iface::host

#endif // KB_INTERFACES_HOST_FAKE_ASYNC_HPP
