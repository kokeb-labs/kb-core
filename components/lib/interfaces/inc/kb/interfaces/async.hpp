// clang-format off: the table below must keep one row per line.
/**
 * @file async.hpp
 * @brief The asynchronous bus contract (ADR-0016): start a transfer, poll it without waiting until
 * it completes or its deadline passes, cancel it; `I2cAsync` and `SpiAsync`, and `AsyncFromSync`,
 * which serves the contract from a synchronous port.
 *
 * | Member | Meaning |
 * | --- | --- |
 * | `kb::Status Start...(..., kb::utils::Deadline deadline) noexcept` | Starts one transfer and returns at once: `Ok` (started), `Busy` (one is in flight), `InvalidArgument` (bad spans or address), `Timeout` (the deadline has passed), `NotReady` |
 * | `AsyncPoll Poll(kb::Instant now) noexcept` | Never waits: `Idle`, `Pending`, or `Done` with the outcome, reported exactly once |
 * | `kb::Status Cancel() noexcept` | Ends the transfer in flight within a bounded time; afterwards nothing touches the buffers and Poll() reports `Idle` |
 * | `static constexpr bool kNonBlocking` | True when Start() and Poll() never wait for the bus (a native port); false for AsyncFromSync |
 *
 * Rules every port keeps:
 *
 * - One transfer in flight per object; the caller's spans stay valid until `Done` is reported or
 *   Cancel() returns.
 * - A transfer that has not completed by its deadline completes as `Timeout`; a completion that
 *   arrives after the deadline is `Timeout` too (late data is never reported as `Ok`).
 * - Cancel() racing a completion: whichever wins, Cancel() returns `Ok`, the completion is
 *   dropped, and the next Poll() is `Idle`.
 * - Completion is signalled from the ISR by the port (an event flag of the owner task, or nothing:
 *   the owner polls once per pass). Poll() itself is callable from one task only, never an ISR.
 *
 * The synchronous contracts (`I2cBus`, `SpiDevice`) are unchanged; an async port is also a sync
 * port, for start-up and slow paths.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
// clang-format on
#ifndef KB_INTERFACES_ASYNC_HPP
#define KB_INTERFACES_ASYNC_HPP

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>

#include "kb/interfaces/i2c.hpp"
#include "kb/interfaces/spi.hpp"
#include "kb/interfaces/time.hpp"
#include "kb/interfaces/types.hpp"
#include "kb/utils/result.hpp"
#include "kb/utils/status.hpp"
#include "kb/utils/time.hpp"

namespace kb::iface {

/** @brief Where an asynchronous transfer is. */
enum class AsyncState : uint8_t {
  Idle = 0,    ///< Nothing in flight (never started, completion already reported, or cancelled).
  Pending = 1, ///< Started, not complete, deadline not passed.
  Done = 2,    ///< Complete: the outcome is in the poll; reported once, then Idle.
};

/** @brief What one Poll() reports. */
struct AsyncPoll {
  AsyncState state{AsyncState::Idle};      ///< Where the transfer is.
  kb::Status status{kb::Status::NotReady}; ///< Done: the outcome; otherwise NotReady.
  uint32_t detail{0U};                     ///< Done with a failure: the vendor code.
  std::size_t count{0U};                   ///< Done with Ok: the bytes moved.

  /** @brief A poll in state `state` with no outcome. */
  [[nodiscard]] static constexpr AsyncPoll InState(AsyncState state) noexcept {
    AsyncPoll p{};
    p.state = state;
    return p;
  }

  /** @brief A completed transfer with `status`, `count` bytes moved, and the vendor `detail`. */
  [[nodiscard]] static constexpr AsyncPoll Completed(kb::Status status, std::size_t count,
                                                     uint32_t detail = 0U) noexcept {
    AsyncPoll p{};
    p.state = AsyncState::Done;
    p.status = status;
    p.count = (status == kb::Status::Ok) ? count : 0U;
    p.detail = detail;
    return p;
  }

  /** @brief True while the transfer is in flight. */
  [[nodiscard]] constexpr bool IsPending() const noexcept {
    return state == AsyncState::Pending;
  }

  /**
   * @brief The completion as a result: the byte count on success, the failure with its detail,
   * `NotReady` when the poll is not `Done`.
   */
  [[nodiscard]] constexpr kb::Result<std::size_t> Completion() const noexcept {
    kb::Result<std::size_t> r = kb::Result<std::size_t>::Failure(kb::Status::NotReady);
    if (state == AsyncState::Done) {
      r = (status == kb::Status::Ok) ? kb::Result<std::size_t>::Success(count)
                                     : kb::Result<std::size_t>::Failure(status, detail);
    }
    return r;
  }
};

/** @brief The part of every asynchronous contract: poll without waiting, cancel, the blocking flag.
 */
template <typename A>
concept AsyncOperation = requires(A& op, kb::Instant now) {
  { op.Poll(now) } noexcept -> std::same_as<AsyncPoll>;
  { op.Cancel() } noexcept -> std::same_as<kb::Status>;
  { A::kNonBlocking } -> std::convertible_to<bool>;
};

/** @brief An I2C controller that also transfers asynchronously. */
template <typename B>
concept I2cAsync = I2cBus<B> && AsyncOperation<B> &&
                   requires(B& bus, uint8_t address, std::span<const uint8_t> tx,
                            std::span<uint8_t> rx, kb::utils::Deadline deadline) {
                     { bus.StartWrite(address, tx, deadline) } noexcept -> std::same_as<kb::Status>;
                     {
                       bus.StartWriteRead(address, tx, rx, deadline)
                     } noexcept -> std::same_as<kb::Status>;
                     { bus.StartRead(address, rx, deadline) } noexcept -> std::same_as<kb::Status>;
                   };

/** @brief An SPI device that also transfers asynchronously (chip select held for the frame). */
template <typename D>
concept SpiAsync = SpiDevice<D> && AsyncOperation<D> &&
                   requires(D& device, std::span<const uint8_t> tx, std::span<uint8_t> rx,
                            kb::utils::Deadline deadline) {
                     {
                       device.StartTransfer(tx, rx, deadline)
                     } noexcept -> std::same_as<kb::Status>;
                   };

/** @brief A native asynchronous port: its Start() and Poll() never wait for the bus. */
template <typename A>
concept NonBlocking = AsyncOperation<A> && A::kNonBlocking;

/**
 * @brief The asynchronous contract over a synchronous port, for ports that have no native form
 * yet (ADR-0016): Start() runs the whole transfer at once (bounded by the port's own timeout) and
 * the next Poll() reports it. It is *not* a hot-path fix: `kNonBlocking` is false, and a handler
 * that must not wait requires `NonBlocking<Port>`.
 *
 * Deadlines are kept as a native port keeps them: a deadline already passed at Start() starts
 * nothing (`Timeout`), and a transfer that ends after its deadline completes as `Timeout`.
 * @tparam Bus An I2cBus or a SpiDevice.
 * @tparam Clock A MonotonicClock (the time a transfer ended).
 */
template <typename Bus, MonotonicClock Clock>
  requires(I2cBus<Bus> || SpiDevice<Bus>)
class AsyncFromSync {
public:
  static constexpr bool kNonBlocking = false; ///< Start() waits for the whole transfer.

  /** @brief Over `bus`, timing with `clock`; no I/O. */
  constexpr AsyncFromSync(Bus& bus, Clock& clock) noexcept : bus_(bus), clock_(clock) {}

  /// @name The synchronous contract, forwarded
  /// @{
  /** @brief Brings the port up. */
  [[nodiscard]] kb::Status EnsureReady() noexcept {
    return bus_.EnsureReady();
  }
  /** @brief I2cBus::Write. */
  [[nodiscard]] kb::Status Write(uint8_t address, std::span<const uint8_t> tx) noexcept
    requires I2cBus<Bus>
  {
    return bus_.Write(address, tx);
  }
  /** @brief I2cBus::WriteRead. */
  [[nodiscard]] kb::Status WriteRead(uint8_t address, std::span<const uint8_t> tx,
                                     std::span<uint8_t> rx) noexcept
    requires I2cBus<Bus>
  {
    return bus_.WriteRead(address, tx, rx);
  }
  /** @brief I2cBus::Read. */
  [[nodiscard]] kb::Status Read(uint8_t address, std::span<uint8_t> rx) noexcept
    requires I2cBus<Bus>
  {
    return bus_.Read(address, rx);
  }
  /** @brief I2cBus::LastFailure. */
  [[nodiscard]] I2cFailure LastFailure() const noexcept
    requires I2cBus<Bus>
  {
    return bus_.LastFailure();
  }
  /** @brief SpiDevice::Transfer. */
  [[nodiscard]] kb::Status Transfer(std::span<const uint8_t> tx, std::span<uint8_t> rx) noexcept
    requires SpiDevice<Bus>
  {
    return bus_.Transfer(tx, rx);
  }
  /// @}

  /// @name The asynchronous contract
  /// @{
  /** @brief Runs a write now; Poll() reports it. */
  [[nodiscard]] kb::Status StartWrite(uint8_t address, std::span<const uint8_t> tx,
                                      kb::utils::Deadline deadline) noexcept
    requires I2cBus<Bus>
  {
    const auto run = [this, address, tx]() noexcept { return bus_.Write(address, tx); };
    return start(run, tx.size(), deadline);
  }
  /** @brief Runs a write-then-read now; Poll() reports it. */
  [[nodiscard]] kb::Status StartWriteRead(uint8_t address, std::span<const uint8_t> tx,
                                          std::span<uint8_t> rx,
                                          kb::utils::Deadline deadline) noexcept
    requires I2cBus<Bus>
  {
    const auto run = [this, address, tx, rx]() noexcept { return bus_.WriteRead(address, tx, rx); };
    return start(run, rx.size(), deadline);
  }
  /** @brief Runs a read now; Poll() reports it. */
  [[nodiscard]] kb::Status StartRead(uint8_t address, std::span<uint8_t> rx,
                                     kb::utils::Deadline deadline) noexcept
    requires I2cBus<Bus>
  {
    const auto run = [this, address, rx]() noexcept { return bus_.Read(address, rx); };
    return start(run, rx.size(), deadline);
  }
  /** @brief Runs a full-duplex frame now; Poll() reports it. */
  [[nodiscard]] kb::Status StartTransfer(std::span<const uint8_t> tx, std::span<uint8_t> rx,
                                         kb::utils::Deadline deadline) noexcept
    requires SpiDevice<Bus>
  {
    const auto run = [this, tx, rx]() noexcept { return bus_.Transfer(tx, rx); };
    return start(run, (tx.size() > rx.size()) ? tx.size() : rx.size(), deadline);
  }
  /** @brief The stored completion, once; then Idle. */
  [[nodiscard]] AsyncPoll Poll(kb::Instant now) noexcept {
    static_cast<void>(now); // the transfer already ended in Start()
    AsyncPoll p = AsyncPoll::InState(AsyncState::Idle);
    if (done_) {
      done_ = false;
      p = result_;
    }
    return p;
  }
  /** @brief Drops a completion not yet reported; always Ok (nothing can be in flight). */
  [[nodiscard]] kb::Status Cancel() noexcept {
    done_ = false;
    return kb::Status::Ok;
  }
  /// @}

private:
  template <typename Run>
  [[nodiscard]] kb::Status start(const Run& run, std::size_t count,
                                 kb::utils::Deadline deadline) noexcept {
    kb::Status status = kb::Status::Busy;
    if (!done_) {
      status = kb::Status::Timeout;
      if (!deadline.Expired(clock_.Now())) {
        const kb::Status outcome = run();
        const bool late = deadline.Expired(clock_.Now());
        result_ = AsyncPoll::Completed(late ? kb::Status::Timeout : outcome, count);
        done_ = true;
        status = kb::Status::Ok;
      }
    }
    return status;
  }

  Bus& bus_;
  Clock& clock_;
  AsyncPoll result_{};
  bool done_{false};
};

} // namespace kb::iface

#endif // KB_INTERFACES_ASYNC_HPP
