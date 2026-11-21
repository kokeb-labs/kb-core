/**
 * @file owner_access.hpp
 * @brief kb::handlers::OwnerAccess<H, Domain, Depth>: model O (ADR-0009). Owns the handler; only
 * the bus's owner task touches it, through `Service()` in the domain's service pass. Other tasks
 * read `Latest()` and `Health()` lock-free and post requests that the owner completes.
 *
 * Each `Service(now)` of the owner task, in order:
 *
 * 1. drains at most `Depth` requests (commands, samples, start, recovery, stop);
 * 2. runs one start-up step while a start is wanted (ADR-0010: the owner task never blocks on a
 *    start), and completes the waiting start and recovery requests when the plan ends;
 * 3. runs the periodic read (`Measure`) when serving and its period is due, and publishes it;
 * 4. beats the liveness feed.
 *
 * The caller API is the same as Guarded's: `EnsureStarted`, `Request`, `Sample`, `Latest`,
 * `Health`, `Startup`, `RequestRecovery`, `Stop`, each bounded by its timeout (`Busy` when no
 * request slot is free, `Timeout` when the owner does not complete in time).
 *
 * Held calls (ADR-0015): the owner task itself, between passes, takes `domain.Hold()` and runs a
 * sequence of calls inline through `Held(hold)` (an `OwnerHeld` with the same `Request` and
 * `Sample`), several handlers of the bus under one hold. Nothing waits: the hold excludes the
 * service pass, so no queued request runs in between. A hold of another bus does not compile.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_HANDLERS_COMMON_OWNER_ACCESS_HPP
#define KB_HANDLERS_COMMON_OWNER_ACCESS_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

#include "kb/handlers/common/bus_domain.hpp"
#include "kb/handlers/common/driver_result.hpp"
#include "kb/handlers/common/facade.hpp"
#include "kb/handlers/common/health.hpp"
#include "kb/handlers/common/liveness.hpp"
#include "kb/handlers/common/request_channel.hpp"
#include "kb/handlers/common/seqlock.hpp"
#include "kb/handlers/common/service_key.hpp"
#include "kb/handlers/common/types.hpp"
#include "kb/rtos/time.hpp"
#include "kb/utils/result.hpp"
#include "kb/utils/status.hpp"
#include "kb/utils/time.hpp"

namespace kb::handlers {

template <typename O>
class OwnerHeld;

/**
 * @brief The owner-task facade of one handler.
 * @tparam H The handler (a HandlerCore).
 * @tparam Domain `BusDomain<Bus, ThreadModel::Owner>` of the handler's bus.
 * @tparam Depth Request slots (1 to 24): callers that may wait at the same time.
 */
template <typename H, typename Domain, std::size_t Depth = 4U>
class OwnerAccess {
  static_assert(Domain::kModel == ThreadModel::Owner,
                "OwnerAccess<H> needs a BusDomain<Bus, ThreadModel::Owner>; this bus is declared "
                "ThreadModel::Guarded: use Guarded<H> (ADR-0009)");
  static_assert(HandlerOnDomain<H, Domain>,
                "the handler's chip is not on this kind of bus (H::kBus and the domain's Bus)");
  static_assert((Depth >= 1U) && (Depth <= 24U), "Depth: 1 to 24 request slots");

public:
  using HandlerType = H;     ///< The handler.
  using DomainType = Domain; ///< The bus domain.

  /** @brief Builds the handler in place from `args`; no I/O, no RTOS call. */
  template <typename... Args>
  explicit OwnerAccess(Domain& domain, Args&&... args) noexcept
      : domain_(domain), handler_(std::forward<Args>(args)...) {}
  OwnerAccess(const OwnerAccess&) = delete;
  OwnerAccess& operator=(const OwnerAccess&) = delete;
  OwnerAccess(OwnerAccess&&) = delete;
  OwnerAccess& operator=(OwnerAccess&&) = delete;
  ~OwnerAccess() = default;

  /** @brief Creates the request queue and flags (after the scheduler runs). */
  [[nodiscard]] kb::Result<void> Create() noexcept {
    return channel_.Create();
  }

  /// @name The caller API (any task but the owner; the same as Guarded)
  /// @{

  /** @brief Asks the owner to start the plan and waits at most `timeout` for it to end. */
  [[nodiscard]] kb::Result<void> EnsureStarted(kb::Milliseconds timeout) noexcept {
    return status(RequestKind::Start, timeout);
  }

  /** @brief Asks the owner to start the plan, without waiting (HAL start-up of many handlers). */
  void RequestStart() noexcept {
    start_wanted_.store(true, std::memory_order_release);
  }

  /** @brief Posts one command and waits at most `timeout` for the owner's reply. */
  [[nodiscard]] kb::Result<typename H::ReplyType> Request(const typename H::CommandType& command,
                                                          kb::Milliseconds timeout) noexcept {
    const kb::Result<uint32_t> slot = channel_.Post(RequestKind::Command, command);
    return slot.IsOk() ? channel_.Wait(slot.Value(), timeout)
                       : Forward<typename H::ReplyType>(slot);
  }

  /** @brief Asks the owner to read now; returns the snapshot it published. */
  [[nodiscard]] kb::Result<Snapshot<typename H::SnapshotType>> Sample(
      kb::Milliseconds timeout) noexcept {
    using S = typename H::SnapshotType;
    const kb::Result<void> done = status(RequestKind::Sample, timeout);
    return done.IsOk() ? handler_.Latest() : Forward<Snapshot<S>>(done);
  }

  /** @brief Asks the owner to run Recover() and the plan; waits at most `timeout` for the end. */
  [[nodiscard]] kb::Result<void> RequestRecovery(kb::Milliseconds timeout) noexcept {
    return status(RequestKind::Recover, timeout);
  }

  /** @brief Asks the owner to deinitialize the handler. */
  [[nodiscard]] kb::Result<void> Stop(kb::Milliseconds timeout) noexcept {
    return status(RequestKind::Stop, timeout);
  }

  /** @brief The newest snapshot, lock-free, with its sequence and age. */
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

  /** @brief The liveness feed (a beat per Service()). */
  [[nodiscard]] const LivenessBeat& Liveness() const noexcept {
    return handler_.Liveness();
  }

  /** @brief The bus domain this handler is on. */
  [[nodiscard]] Domain& BusDomainOf() noexcept {
    return domain_;
  }

  /** @brief The request queue's counters. */
  [[nodiscard]] ChannelStats Requests() const noexcept {
    return channel_.Stats();
  }

  /// @}
  /// @name The owner task (through BusDomain::Service)
  /// @{

  /**
   * @brief Calls of the handler run inline under `hold`, a hold of this handler's bus that the
   * owner task took between passes (`domain.Hold()`).
   * @return The held calls; they return `NotReady` and send nothing while `hold` does not own this
   * handler's bus (not had, released, or taken on another domain object of the same type).
   */
  [[nodiscard]] OwnerHeld<OwnerAccess> Held(BusHold<Domain>& hold) noexcept {
    return OwnerHeld<OwnerAccess>{*this, hold};
  }

  /** @brief One service of the handler at `now` (see the file note). Always true. */
  [[nodiscard]] bool Service(const ServiceKey& key, kb::Instant now) noexcept {
    drain(key, now);
    if (start_wanted_.load(std::memory_order_acquire)) {
      startStep(key, now);
    } else if (sampleDue(now)) {
      static_cast<void>(handler_.Sample(key, now));
      last_sample_ = now;
      sampled_ = true;
    } else {
      // Nothing periodic this pass.
    }
    handler_.Beat(key, now);
    return true;
  }

  /// @}

private:
  friend class OwnerHeld<OwnerAccess>;

  /// Runs one operation of the handler inline: the caller holds the owner bus.
  template <typename Op, typename... Args>
  [[nodiscard]] auto runHeld(Op op, Args&&... args) noexcept
      -> std::invoke_result_t<Op, H&, const ServiceKey&, Args...> {
    const ServiceKey key{};
    auto result = op(handler_, key, std::forward<Args>(args)...);
    handler_.Beat(key, kb::rtos::Now());
    return result;
  }

  [[nodiscard]] kb::Result<void> status(RequestKind kind, kb::Milliseconds timeout) noexcept {
    const typename H::CommandType none{};
    const kb::Result<uint32_t> slot = channel_.Post(kind, none);
    kb::Result<void> result = Forward<void>(slot);
    if (slot.IsOk()) {
      const kb::Result<typename H::ReplyType> reply = channel_.Wait(slot.Value(), timeout);
      result = reply.IsOk() ? kb::Result<void>::Success() : Forward<void>(reply);
    }
    return result;
  }

  void drain(const ServiceKey& key, kb::Instant now) noexcept {
    using R = typename H::ReplyType;
    TakenRequest<typename H::CommandType> request{};
    for (std::size_t n = 0U; n < Depth; ++n) {
      if (channel_.Next(request)) {
        switch (request.kind) { // GCOVR_EXCL_BR_LINE: kinds come only from this facade
        case RequestKind::Command:
          channel_.Complete(request.slot, handler_.Execute(key, request.command, now));
          break;
        case RequestKind::Sample:
          channel_.Complete(request.slot, asReply(handler_.Sample(key, now)));
          break;
        case RequestKind::Start:
          wait(start_waiters_, request.slot);
          start_wanted_.store(true, std::memory_order_release);
          break;
        case RequestKind::Recover:
          recover(key, now, request.slot);
          break;
        case RequestKind::Stop:
          start_wanted_.store(false, std::memory_order_release);
          channel_.Complete(request.slot, asReply(handler_.Deinitialize(key)));
          completeWaiters(kb::Result<R>::Failure(kb::Status::NotReady));
          break;
        }
      }
    }
  }

  void recover(const ServiceKey& key, kb::Instant now, uint32_t slot) noexcept {
    const kb::Result<void> began = handler_.Recover(key, now);
    if (began.IsOk()) {
      wait(start_waiters_, slot);
      start_wanted_.store(true, std::memory_order_release);
    } else {
      channel_.Complete(slot, asReply(began));
    }
  }

  void startStep(const ServiceKey& key, kb::Instant now) noexcept {
    using R = typename H::ReplyType;
    const kb::Result<StartProgress> step = handler_.StartStep(key, now);
    const bool ended = (!step.IsOk()) || (step.Value() == StartProgress::Done);
    if (ended) {
      start_wanted_.store(false, std::memory_order_release);
      completeWaiters(step.IsOk() ? kb::Result<R>::Success(R{}) : Forward<R>(step));
    }
  }

  void completeWaiters(const kb::Result<typename H::ReplyType>& reply) noexcept {
    for (std::size_t i = 0U; i < Depth; ++i) {
      const uint32_t bit = (uint32_t{1U} << static_cast<uint32_t>(i));
      if ((start_waiters_ & bit) != 0U) {
        channel_.Complete(static_cast<uint32_t>(i), reply);
      }
    }
    start_waiters_ = 0U;
  }

  static void wait(uint32_t& waiters, uint32_t slot) noexcept {
    waiters |= (uint32_t{1U} << slot);
  }

  template <typename T>
  [[nodiscard]] static kb::Result<typename H::ReplyType> asReply(const kb::Result<T>& r) noexcept {
    using R = typename H::ReplyType;
    return r.IsOk() ? kb::Result<R>::Success(R{}) : Forward<R>(r);
  }

  [[nodiscard]] bool sampleDue(kb::Instant now) const noexcept {
    const kb::Result<HealthRecord> health = handler_.Health();
    // The owner task is the only writer, so its own read is never torn.
    const bool serving = health.IsOk() && IsServing(health.Value().state); // GCOVR_EXCL_BR_LINE
    const kb::Milliseconds period = handler_.Settings().sample_period;
    const bool due = (!sampled_) || (period.Count() == 0U) ||
                     kb::utils::Deadline::After(last_sample_, period).Expired(now);
    return serving && H::Measures() && due;
  }

  Domain& domain_;
  H handler_;
  RequestChannel<typename H::CommandType, typename H::ReplyType, Depth> channel_{};
  std::atomic<bool> start_wanted_{false};
  kb::Instant last_sample_;
  uint32_t start_waiters_{0U};
  bool sampled_{false};
};

/**
 * @brief Calls of one owner-model handler under the owner task's hold of its bus
 * (OwnerAccess::Held). Not copyable or movable; it must not outlive the hold it borrows.
 * @tparam O The OwnerAccess facade.
 */
template <typename O>
class [[nodiscard]] OwnerHeld {
  using H = typename O::HandlerType;
  using Domain = typename O::DomainType;

public:
  OwnerHeld(const OwnerHeld&) = delete;
  OwnerHeld& operator=(const OwnerHeld&) = delete;
  OwnerHeld(OwnerHeld&&) = delete;
  OwnerHeld& operator=(OwnerHeld&&) = delete;
  ~OwnerHeld() = default;

  /** @brief True while the borrowed hold owns this handler's bus. */
  [[nodiscard]] bool OwnsLock() const noexcept {
    return hold_.OwnsLock() && hold_.IsOn(o_.domain_);
  }

  /** @brief Runs one command now, inline (`timeout` unused: nothing waits). */
  [[nodiscard]] kb::Result<typename H::ReplyType> Request(const typename H::CommandType& command,
                                                          kb::Milliseconds timeout) noexcept {
    static_cast<void>(timeout); // inline on the owner task; the call is bounded by the driver
    kb::Result<typename H::ReplyType> result =
        kb::Result<typename H::ReplyType>::Failure(kb::Status::NotReady);
    if (OwnsLock()) {
      result = o_.runHeld(ops::Execute{}, command);
    }
    return result;
  }

  /** @brief Runs the periodic read now and returns the snapshot it published. */
  [[nodiscard]] kb::Result<Snapshot<typename H::SnapshotType>> Sample(
      kb::Milliseconds timeout) noexcept {
    static_cast<void>(timeout); // inline on the owner task; the call is bounded by the driver
    using S = Snapshot<typename H::SnapshotType>;
    kb::Result<S> result = kb::Result<S>::Failure(kb::Status::NotReady);
    if (OwnsLock()) {
      const kb::Result<typename H::SnapshotType> sampled = o_.runHeld(ops::Sample{});
      result = sampled.IsOk() ? o_.Latest() : Forward<S>(sampled);
    }
    return result;
  }

  /** @brief The health record, lock-free. */
  [[nodiscard]] kb::Result<HealthRecord> Health() const noexcept {
    return o_.Health();
  }

private:
  friend O;

  OwnerHeld(O& o, BusHold<Domain>& hold) noexcept : o_(o), hold_(hold) {}

  O& o_;
  BusHold<Domain>& hold_;
};

} // namespace kb::handlers

#endif // KB_HANDLERS_COMMON_OWNER_ACCESS_HPP
