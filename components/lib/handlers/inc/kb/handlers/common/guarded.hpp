/**
 * @file guarded.hpp
 * @brief kb::handlers::Guarded<H, Domain>: model G (ADR-0009). Owns the handler and is the only way
 * to reach it; every operation takes the handler's mutex, then the bus mutex, both with a bounded
 * `TryLockFor`, runs the single-threaded call and releases both.
 *
 * - The lock order is fixed (handler, then bus); no lock is held across another handler or a user
 *   callback (an operation runs one handler call). A call that cannot get both locks in time
 *   returns `Timeout` and sends nothing.
 * - `EnsureStarted()` runs the start-up plan under both locks, bounded by the sum of the plan's
 *   stage deadlines (ADR-0010); `RequestRecovery()` runs `Recover()` and the plan the same way.
 * - The caller API is the same as OwnerAccess's: `EnsureStarted`, `Request`, `Sample`, `Latest`,
 *   `Health`, `Startup`, `RequestRecovery`, `Stop`, each bounded by its timeout.
 * - A handler with `kRequiresOwner` (a chip that must be serviced continuously), a domain that is
 * not `ThreadModel::Guarded`, or a bus of another kind than the handler's chip does not compile.
 * - `Hold(timeout)` keeps both locks for a sequence of calls (ADR-0016): a `GuardedHeld` has the
 *   same `Request` and `Sample` as the facade, so a binding's app call (an EEPROM range of many
 *   chunks) runs whole, with no other task's call in between. It takes the locks in the same order
 *   as every call; there is no way to make one from a `BusHold` taken first (that order would
 * invert handler-then-bus).
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_HANDLERS_COMMON_GUARDED_HPP
#define KB_HANDLERS_COMMON_GUARDED_HPP

#include <atomic>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>

#include "kb/handlers/common/bus_domain.hpp"
#include "kb/handlers/common/driver_result.hpp"
#include "kb/handlers/common/facade.hpp"
#include "kb/handlers/common/health.hpp"
#include "kb/handlers/common/liveness.hpp"
#include "kb/handlers/common/seqlock.hpp"
#include "kb/handlers/common/service_key.hpp"
#include "kb/handlers/common/types.hpp"
#include "kb/rtos/mutex.hpp"
#include "kb/rtos/time.hpp"
#include "kb/utils/result.hpp"
#include "kb/utils/status.hpp"
#include "kb/utils/time.hpp"

namespace kb::handlers {

/** @brief A result type the facades can fail with Timeout: `kb::Result<T>`. */
template <typename R>
concept FacadeResult = requires(const R& r) {
  { R::Failure(kb::Status::Timeout) } noexcept -> std::same_as<R>;
  { r.IsOk() } noexcept -> std::same_as<bool>;
};

template <typename G>
class GuardedHeld;

/**
 * @brief The guarded facade of one handler.
 * @tparam H The handler (a HandlerCore).
 * @tparam Domain `BusDomain<Bus, ThreadModel::Guarded>` of the handler's bus.
 */
template <typename H, typename Domain>
class Guarded {
  static_assert(Domain::kModel == ThreadModel::Guarded,
                "Guarded<H> needs a BusDomain<Bus, ThreadModel::Guarded>; this bus is declared "
                "ThreadModel::Owner: use OwnerAccess<H> (ADR-0009)");
  static_assert(!H::kRequiresOwner,
                "this chip must be serviced continuously (kRequiresOwner): put it on an owner "
                "bus and use OwnerAccess<H> (ADR-0009)");
  static_assert(HandlerOnDomain<H, Domain>,
                "the handler's chip is not on this kind of bus (H::kBus and the domain's Bus)");

public:
  using HandlerType = H;     ///< The handler.
  using DomainType = Domain; ///< The bus domain.

  /** @brief Builds the handler in place from `args`; no I/O, no RTOS call. */
  template <typename... Args>
  explicit Guarded(Domain& domain, Args&&... args) noexcept
      : domain_(domain), handler_(std::forward<Args>(args)...) {}
  Guarded(const Guarded&) = delete;
  Guarded& operator=(const Guarded&) = delete;
  Guarded(Guarded&&) = delete;
  Guarded& operator=(Guarded&&) = delete;
  ~Guarded() = default;

  /** @brief Creates the handler's mutex (after the scheduler runs). */
  [[nodiscard]] kb::Result<void> Create() noexcept {
    return mutex_.Create();
  }

  /**
   * @brief Runs `op(handler, key, args...)` holding the handler's mutex, then the bus mutex, each
   * waited for within what is left of `timeout`.
   * @return The operation's result; `Timeout` (nothing sent) when a lock was not had in time.
   */
  template <typename Op, typename... Args>
  [[nodiscard]] auto Call(kb::Milliseconds timeout, Op op, Args&&... args) noexcept
      -> std::invoke_result_t<Op, H&, const ServiceKey&, Args...> {
    using R = std::invoke_result_t<Op, H&, const ServiceKey&, Args...>;
    static_assert(FacadeResult<R>, "a guarded operation returns a kb::Result");
    R result = R::Failure(kb::Status::Timeout);
    const kb::Instant start = kb::rtos::Now();
    const kb::rtos::LockGuard handler_lock{mutex_, timeout};
    if (handler_lock.OwnsLock()) {
      const kb::Milliseconds left =
          Remaining(timeout, kb::utils::ElapsedUs(kb::rtos::Now(), start));
      const BusHold<Domain> bus_lock = domain_.Hold(left);
      if (bus_lock.OwnsLock()) {
        result = runHeld(op, std::forward<Args>(args)...);
      } else {
        countTimeout();
      }
    } else {
      countTimeout();
    }
    return result;
  }

  /// @name The caller API (the same as OwnerAccess)
  /// @{

  /** @brief Runs the start-up plan (bounded by its deadlines); Ok at once when started. */
  [[nodiscard]] kb::Result<void> EnsureStarted(kb::Milliseconds timeout) noexcept {
    return Call(timeout, ops::Start{});
  }

  /** @brief Runs one command of the handler. */
  [[nodiscard]] kb::Result<typename H::ReplyType> Request(const typename H::CommandType& command,
                                                          kb::Milliseconds timeout) noexcept {
    return Call(timeout, ops::Execute{}, command);
  }

  /** @brief Runs the periodic read now and returns the snapshot it published. */
  [[nodiscard]] kb::Result<Snapshot<typename H::SnapshotType>> Sample(
      kb::Milliseconds timeout) noexcept {
    const kb::Result<typename H::SnapshotType> sampled = Call(timeout, ops::Sample{});
    return sampled.IsOk() ? handler_.Latest()
                          : Forward<Snapshot<typename H::SnapshotType>>(sampled);
  }

  /** @brief Runs Recover() and the start-up plan again (backoff applies). */
  [[nodiscard]] kb::Result<void> RequestRecovery(kb::Milliseconds timeout) noexcept {
    return Call(timeout, ops::Recover{});
  }

  /** @brief Deinitializes the handler. */
  [[nodiscard]] kb::Result<void> Stop(kb::Milliseconds timeout) noexcept {
    return Call(timeout, ops::Stop{});
  }

  /** @brief The newest snapshot, lock-free. */
  [[nodiscard]] kb::Result<Snapshot<typename H::SnapshotType>> Latest() const noexcept {
    return handler_.Latest();
  }

  /** @brief The health record, lock-free. */
  [[nodiscard]] kb::Result<HealthRecord> Health() const noexcept {
    return handler_.Health();
  }

  /** @brief The report of the last start-up run, lock-free. */
  [[nodiscard]] kb::Result<StartupReport> Startup() const noexcept {
    return handler_.Startup();
  }

  /** @brief The liveness feed (a beat per call that ran). */
  [[nodiscard]] const LivenessBeat& Liveness() const noexcept {
    return handler_.Liveness();
  }

  /// @}

  /**
   * @brief Holds the handler and its bus for a sequence of calls: the handler's mutex, then the
   * bus, each waited for within what is left of `timeout` (the order of every call).
   * @return The hold; check OwnsLock(). Its calls return `Timeout` when it was not had.
   */
  [[nodiscard]] GuardedHeld<Guarded> Hold(kb::Milliseconds timeout) noexcept {
    return GuardedHeld<Guarded>{*this, timeout};
  }

  /** @brief Calls refused because a lock was not had in time. */
  [[nodiscard]] uint32_t LockTimeouts() const noexcept {
    return timeouts_.load(std::memory_order_relaxed);
  }

private:
  friend class GuardedHeld<Guarded>;

  /// Runs one operation of the handler: both locks are held by the caller.
  template <typename Op, typename... Args>
  [[nodiscard]] auto runHeld(Op op, Args&&... args) noexcept
      -> std::invoke_result_t<Op, H&, const ServiceKey&, Args...> {
    const ServiceKey key{};
    auto result = op(handler_, key, std::forward<Args>(args)...);
    handler_.Beat(key, kb::rtos::Now());
    return result;
  }

  void countTimeout() noexcept {
    static_cast<void>(timeouts_.fetch_add(1U, std::memory_order_relaxed));
  }

  Domain& domain_;
  H handler_;
  kb::rtos::Mutex mutex_;
  std::atomic<uint32_t> timeouts_{0U};
};

/**
 * @brief Both locks of one guarded handler, held for a sequence of calls (Guarded::Hold).
 *
 * Not copyable or movable: it lives in the scope that took it. `Release()` (or the destructor)
 * gives the bus back, then the handler. Calls on a hold that was not had return `Timeout`; after
 * `Release()` they return `NotReady`; neither sends anything.
 * @tparam G The Guarded facade.
 */
template <typename G>
class [[nodiscard]] GuardedHeld {
  using H = typename G::HandlerType;
  using Domain = typename G::DomainType;

public:
  GuardedHeld(const GuardedHeld&) = delete;
  GuardedHeld& operator=(const GuardedHeld&) = delete;
  GuardedHeld(GuardedHeld&&) = delete;
  GuardedHeld& operator=(GuardedHeld&&) = delete;
  /** @brief Releases both locks when held. */
  ~GuardedHeld() {
    Release();
  }

  /** @brief True while the handler and its bus are held. */
  [[nodiscard]] bool OwnsLock() const noexcept {
    return owns_;
  }

  /** @brief Gives the bus back, then the handler; nothing when not held. */
  void Release() noexcept {
    if (owns_) {
      owns_ = false;
      bus_.reset();
      g_.mutex_.Unlock();
    }
  }

  /** @brief Runs one command of the handler under the hold (`timeout` unused: the hold is had). */
  [[nodiscard]] kb::Result<typename H::ReplyType> Request(const typename H::CommandType& command,
                                                          kb::Milliseconds timeout) noexcept {
    static_cast<void>(timeout); // the locks are held; the call is bounded by the driver
    kb::Result<typename H::ReplyType> result = refused<typename H::ReplyType>();
    if (owns_) {
      result = g_.runHeld(ops::Execute{}, command);
    }
    return result;
  }

  /** @brief Runs the periodic read under the hold and returns the snapshot it published. */
  [[nodiscard]] kb::Result<Snapshot<typename H::SnapshotType>> Sample(
      kb::Milliseconds timeout) noexcept {
    static_cast<void>(timeout); // the locks are held; the call is bounded by the driver
    using S = Snapshot<typename H::SnapshotType>;
    kb::Result<S> result = refused<S>();
    if (owns_) {
      const kb::Result<typename H::SnapshotType> sampled = g_.runHeld(ops::Sample{});
      result = sampled.IsOk() ? g_.Latest() : Forward<S>(sampled);
    }
    return result;
  }

  /** @brief The health record, lock-free. */
  [[nodiscard]] kb::Result<HealthRecord> Health() const noexcept {
    return g_.Health();
  }

private:
  friend G;

  GuardedHeld(G& g, kb::Milliseconds timeout) noexcept : g_(g) {
    const kb::Instant start = kb::rtos::Now();
    if (g_.mutex_.TryLockFor(timeout)) {
      const kb::Milliseconds left =
          Remaining(timeout, kb::utils::ElapsedUs(kb::rtos::Now(), start));
      owns_ = bus_.emplace(g_.domain_.Hold(left)).OwnsLock();
      if (!owns_) {
        bus_.reset();
        g_.mutex_.Unlock();
      }
    }
    acquired_ = owns_;
    if (!owns_) {
      g_.countTimeout();
    }
  }

  template <typename T>
  [[nodiscard]] kb::Result<T> refused() const noexcept {
    return kb::Result<T>::Failure(acquired_ ? kb::Status::NotReady : kb::Status::Timeout);
  }

  G& g_;
  std::optional<BusHold<Domain>> bus_{};
  bool owns_{false};
  bool acquired_{false};
};

} // namespace kb::handlers

#endif // KB_HANDLERS_COMMON_GUARDED_HPP
