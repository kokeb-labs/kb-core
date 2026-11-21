/**
 * @file handler_core.hpp
 * @brief kb::handlers::HandlerCore<Derived, Driver, ...>: the lifecycle, the start-up pipeline
 * runner (ADR-0010), the health record and the published snapshot of every handler, written once
 * (CRTP, ADR-0006 register).
 *
 * The core is single-threaded by construction (ADR-0009): it holds no lock, and every call that
 * can change it takes a `ServiceKey`, which only the access facades can make. Construction stores
 * the configuration, does no I/O, touches no RTOS object and cannot fail.
 *
 * What the derived handler (the binding) supplies, public or reachable by friendship:
 *
 * | Member | Use |
 * | --- | --- |
 * | `static constexpr std::array<Stage, N> kStartupPlan` | The declared stages, in kind order |
 * | `static constexpr bool kRequiresOwner` | True for a chip that must be serviced continuously |
 * | `static constexpr BusKind kBus` | The bus its chip is on |
 * | `void EmplaceDriver(std::optional<Driver>&) noexcept` | Constructs the driver (no I/O) |
 * | `kb::Result<StartProgress> RunStage(const StageCall&, Driver&) noexcept` | One step of a stage
 * | | `kb::Milliseconds StageDeadline(std::size_t) const noexcept` | From the binding's config | |
 * optional `bool StageEnabled(std::size_t) const noexcept` | False skips an optional stage | |
 * optional `void AbortStartup(Driver&) noexcept` | Runs when a stage fails | | optional
 * `kb::Result<void> StopDriver(Driver&) noexcept` | `Deinitialize()` of the driver | | optional
 * `kb::Result<void> RecoverDriver(Driver&, kb::Instant) noexcept` | The plan's recovery | |
 * optional `bool IsFatal(kb::Status, uint32_t) const noexcept` | A failure that faults at once | |
 * optional `bool CountsAgainstHealth(kb::Status, uint32_t) const noexcept` | False for a caller's
 * mistake (default: InvalidArgument) | | optional `kb::Result<SnapshotT> Measure(Driver&,
 * kb::Instant) noexcept` | The periodic read | | optional `kb::Result<ReplyT> Run(Driver&, const
 * CommandT&, kb::Instant) noexcept` | A command |
 *
 * Rules kept here: `EnsureInitialized()` (or `StartStep()`) is the only start; `Execute()` and
 * `Sample()` never start or recover the chip, and before a successful start they return
 * `NotReady` with no driver call; `Recover()` runs only through a facade (owner task or guarded
 * locks), with backoff.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_HANDLERS_COMMON_HANDLER_CORE_HPP
#define KB_HANDLERS_COMMON_HANDLER_CORE_HPP

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

#include "kb/handlers/common/driver_result.hpp"
#include "kb/handlers/common/health.hpp"
#include "kb/handlers/common/liveness.hpp"
#include "kb/handlers/common/seqlock.hpp"
#include "kb/handlers/common/service_key.hpp"
#include "kb/handlers/common/types.hpp"
#include "kb/rtos/time.hpp"
#include "kb/utils/result.hpp"
#include "kb/utils/status.hpp"
#include "kb/utils/time.hpp"

namespace kb::handlers {

/** @brief The snapshot type of a handler without a periodic read. */
struct NoSnapshot {
  uint8_t unused{0U}; ///< Placeholder.
};

/** @brief The command type of a handler without commands. */
struct NoCommand {
  uint8_t unused{0U}; ///< Placeholder.
};

/** @brief The reply type of a command without a value. */
struct NoReply {
  uint8_t unused{0U}; ///< Placeholder.
};

/** @brief What a binding's RunStage() is told about the step it runs. */
struct StageCall {
  std::size_t index{0U}; ///< The stage of kStartupPlan.
  kb::Instant now;       ///< This step's time.
  kb::Instant started;   ///< When this stage's first step ran.
  uint32_t calls{0U};    ///< Steps of this stage before this one (0 on the first).
};

/** @brief The handler settings common to every chip; board values the HAL supplies. */
struct HandlerConfig {
  HealthPolicy health{};              ///< When health degrades and faults.
  RecoveryPolicy recovery{};          ///< Backoff and attempts of Recover().
  kb::Milliseconds poll{1U};          ///< Wait between polls in EnsureInitialized() (>= 1 used).
  kb::Milliseconds sample_period{0U}; ///< Owner model: Measure() period; 0 = every Service().
};

/**
 * @brief The CRTP base of every handler.
 * @tparam Derived The handler (the binding's handler class).
 * @tparam Driver The driver class, owned in a `std::optional` and built by the start-up pipeline.
 * @tparam SnapshotT What `Measure()` publishes (`Latest()`); NoSnapshot when nothing.
 * @tparam CommandT What `Run()` takes (`Request()`); NoCommand when nothing.
 * @tparam ReplyT What `Run()` returns; NoReply when nothing.
 */
template <typename Derived, typename Driver, SnapshotValue SnapshotT = NoSnapshot,
          SnapshotValue CommandT = NoCommand, SnapshotValue ReplyT = NoReply>
class HandlerCore {
public:
  using DriverType = Driver;      ///< The driver class.
  using SnapshotType = SnapshotT; ///< What Latest() returns.
  using CommandType = CommandT;   ///< What Request() takes.
  using ReplyType = ReplyT;       ///< What Request() returns.

  HandlerCore(const HandlerCore&) = delete;
  HandlerCore& operator=(const HandlerCore&) = delete;
  HandlerCore(HandlerCore&&) = delete;
  HandlerCore& operator=(HandlerCore&&) = delete;

  /// @name Lifecycle (one context: the owner task, or under the guarded locks)
  /// @{

  /**
   * @brief Runs at most one step of the current start-up stage at `now` (ADR-0010).
   * @return Done once the plan completed (at once, with no I/O, when already started); Pending when
   * another step is needed; a failure names the stage in Startup(), destroys the driver, and the
   * next call starts again from the first stage. A handler that faulted while running is not
   * restarted here: `Recover()` does that (NotReady).
   */
  [[nodiscard]] kb::Result<StartProgress> StartStep(const ServiceKey& key,
                                                    kb::Instant now) noexcept {
    static_cast<void>(key);
    static_assert(IsValidPlan(Derived::kStartupPlan),
                  "kStartupPlan: 1 to 8 stages, known kinds, each stage after the previous in kind "
                  "order (ADR-0010)");
    kb::Result<StartProgress> result = kb::Result<StartProgress>::Success(StartProgress::Done);
    const HealthState state = health_.Current().state;
    if (IsServing(state)) {
      result = kb::Result<StartProgress>::Success(StartProgress::Done);
    } else if ((state == HealthState::Faulted) && driver_.has_value()) {
      result = kb::Result<StartProgress>::Failure(kb::Status::NotReady);
    } else {
      if (!running_) {
        beginRun(now);
      }
      result = step(now);
    }
    return result;
  }

  /**
   * @brief The one start: the loop over StartStep(), waiting `poll` between steps with
   * kb::rtos::SleepFor, bounded by the sum of the stage deadlines. Idempotent: Ok at once when
   * started.
   */
  [[nodiscard]] kb::Result<void> EnsureInitialized(const ServiceKey& key) noexcept {
    const kb::Instant begin = kb::rtos::Now();
    const kb::utils::Deadline bound = kb::utils::Deadline::After(begin, budget());
    kb::Result<StartProgress> progress = StartStep(key, begin);
    while (progress.IsOk() && (progress.Value() == StartProgress::Pending)) {
      if (bound.Expired(kb::rtos::Now())) {
        progress = kb::Result<StartProgress>::Failure(kb::Status::Timeout);
        failRun(kb::Status::Timeout, 0U, kb::rtos::Now());
      } else {
        kb::rtos::SleepFor(pollInterval());
        progress = StartStep(key, kb::rtos::Now());
      }
    }
    return progress.IsOk() ? kb::Result<void>::Success() : Forward<void>(progress);
  }

  /** @brief Stops the driver (the binding's StopDriver) and destroys it; health Unknown. */
  [[nodiscard]] kb::Result<void> Deinitialize(const ServiceKey& key) noexcept {
    static_cast<void>(key);
    kb::Result<void> result = kb::Result<void>::Success();
    if (driver_.has_value()) {
      result = stopDriver(*driver_);
      driver_.reset();
    }
    running_ = false;
    recovering_ = false;
    health_.Stopped();
    return result;
  }

  /**
   * @brief Starts the plan's recovery at `now`: the binding's RecoverDriver() (generic: the
   * driver's Deinitialize()), the driver destroyed, then the start-up plan again, driven by
   * StartStep() (owner task) or EnsureInitialized() (guarded facade).
   * @return Ok when the recovery began; NotReady when never started; Busy inside the backoff;
   * Fault when RecoveryPolicy::max_attempts failed attempts in a row were reached.
   */
  [[nodiscard]] kb::Result<void> Recover(const ServiceKey& key, kb::Instant now) noexcept {
    static_cast<void>(key);
    kb::Result<void> result = kb::Result<void>::Success();
    const RecoveryPolicy& policy = config_.recovery;
    const bool exhausted = (policy.max_attempts != 0U) && (attempts_ >= policy.max_attempts);
    if (health_.Current().state == HealthState::Unknown) {
      result = kb::Result<void>::Failure(kb::Status::NotReady);
    } else if (exhausted) {
      result = kb::Result<void>::Failure(kb::Status::Fault);
    } else if ((attempts_ != 0U) && (!backoff_.Expired(now))) {
      result = kb::Result<void>::Failure(kb::Status::Busy);
    } else {
      if (driver_.has_value()) {
        static_cast<void>(recoverDriver(*driver_, now));
        driver_.reset();
      }
      running_ = false;
      recovering_ = true;
      health_.BeginStart(true);
    }
    return result;
  }

  /// @}
  /// @name Served calls (never start or recover; NotReady before a start)
  /// @{

  /** @brief Runs the binding's command at `now` and records the outcome in the health record. */
  [[nodiscard]] kb::Result<ReplyT> Execute(const ServiceKey& key, const CommandT& command,
                                           kb::Instant now) noexcept {
    static_cast<void>(key);
    kb::Result<ReplyT> result = kb::Result<ReplyT>::Failure(kb::Status::NotReady);
    if (!Runs()) {
      result = kb::Result<ReplyT>::Failure(kb::Status::Unsupported);
    } else if (serving() && driver_.has_value()) {
      result = runCommand(*driver_, command, now);
      record(result.Error(), result.Detail(), now);
    } else {
      // Not started, or faulted: nothing is sent.
    }
    return result;
  }

  /** @brief Runs the binding's periodic read at `now`, records it and publishes the snapshot. */
  [[nodiscard]] kb::Result<SnapshotT> Sample(const ServiceKey& key, kb::Instant now) noexcept {
    static_cast<void>(key);
    kb::Result<SnapshotT> result = kb::Result<SnapshotT>::Failure(kb::Status::NotReady);
    if (!Measures()) {
      result = kb::Result<SnapshotT>::Failure(kb::Status::Unsupported);
    } else if (serving() && driver_.has_value()) {
      result = measure(*driver_, now);
      record(result.Error(), result.Detail(), now);
      if (result.IsOk()) {
        Stamped<SnapshotT> stamped{};
        stamped.value = result.Value();
        stamped.at_us = now.Us();
        stamped.valid = true;
        snapshot_.Publish(stamped);
      }
    } else {
      // Not started, or faulted: nothing is sent.
    }
    return result;
  }

  /** @brief Marks the handler Faulted at `now` without a call (a reset event, a stuck bus). */
  void Fault(const ServiceKey& key, kb::Status status, uint32_t detail, kb::Instant now) noexcept {
    static_cast<void>(key);
    health_.Fault(status, detail, now);
  }

  /** @brief Records a beat of the liveness feed (facades call it once per service or call). */
  void Beat(const ServiceKey& key, kb::Instant now) noexcept {
    static_cast<void>(key);
    liveness_.Beat(now);
  }

  /// @}
  /// @name Lock-free reads (any task)
  /// @{

  /** @brief The newest snapshot with its sequence and age; NotReady before the first one. */
  [[nodiscard]] kb::Result<Snapshot<SnapshotT>> Latest() const noexcept {
    kb::Result<Snapshot<SnapshotT>> result =
        kb::Result<Snapshot<SnapshotT>>::Failure(kb::Status::NotReady);
    Stamped<SnapshotT> stamped{};
    uint32_t publishes = 0U;
    bool clean = false;
    for (uint32_t attempt = 0U; attempt < kReadAttempts; ++attempt) {
      if (!clean) {
        clean = snapshot_.TryRead(stamped, publishes);
      }
    }
    if (!clean) {
      result = kb::Result<Snapshot<SnapshotT>>::Failure(kb::Status::Busy);
    } else if (stamped.valid) {
      Snapshot<SnapshotT> snap{};
      snap.value = stamped.value;
      snap.sequence = publishes;
      snap.age_us = kb::utils::ElapsedUs(kb::rtos::Now(), kb::Instant{stamped.at_us});
      result = kb::Result<Snapshot<SnapshotT>>::Success(snap);
    } else {
      result = kb::Result<Snapshot<SnapshotT>>::Failure(kb::Status::NotReady);
    }
    return result;
  }

  /** @brief A copy of the health record; Busy only when every bounded attempt was torn. */
  [[nodiscard]] kb::Result<HealthRecord> Health() const noexcept {
    return health_.Read();
  }

  /** @brief A copy of the report of the last start-up run. */
  [[nodiscard]] kb::Result<StartupReport> Startup() const noexcept {
    kb::Result<StartupReport> result = kb::Result<StartupReport>::Success(StartupReport{});
    if (report_.Publishes() != 0U) {
      result = report_.Read();
    }
    return result;
  }

  /** @brief The liveness feed for a LivenessMonitor. */
  [[nodiscard]] const LivenessBeat& Liveness() const noexcept {
    return liveness_;
  }

  /** @brief The handler settings. */
  [[nodiscard]] const HandlerConfig& Settings() const noexcept {
    return config_;
  }

  /// @}

  /** @brief True when the binding has commands (`Run`). */
  [[nodiscard]] static constexpr bool Runs() noexcept {
    return requires(Derived& d, Driver& driver, const CommandT& command, kb::Instant now) {
      { d.Run(driver, command, now) } noexcept -> std::same_as<kb::Result<ReplyT>>;
    };
  }

  /** @brief True when the binding has a periodic read (`Measure`). */
  [[nodiscard]] static constexpr bool Measures() noexcept {
    return requires(Derived& d, Driver& driver, kb::Instant now) {
      { d.Measure(driver, now) } noexcept -> std::same_as<kb::Result<SnapshotT>>;
    };
  }

protected:
  /** @brief Stores the settings; no I/O, no RTOS call. */
  constexpr explicit HandlerCore(const HandlerConfig& config) noexcept : config_(config) {
    health_.SetPolicy(config.health);
  }
  ~HandlerCore() = default;

  /** @brief The driver while one exists (for the derived handler's own helpers). */
  [[nodiscard]] Driver* DriverPointer() noexcept {
    return driver_.has_value() ? &(*driver_) : nullptr;
  }

private:
  // The stages of the plan (Derived is complete wherever this is called).
  [[nodiscard]] static constexpr std::size_t stageCount() noexcept {
    return Derived::kStartupPlan.size();
  }

  [[nodiscard]] Derived& derived() noexcept {
    return static_cast<Derived&>(*this);
  }
  [[nodiscard]] const Derived& derived() const noexcept {
    return static_cast<const Derived&>(*this);
  }

  [[nodiscard]] bool serving() const noexcept {
    return IsServing(health_.Current().state);
  }

  [[nodiscard]] kb::Milliseconds pollInterval() const noexcept {
    return (config_.poll.Count() == 0U) ? kb::Milliseconds{1U} : config_.poll;
  }

  // The sum of every enabled stage's deadline plus one poll per stage: the bound of one start.
  [[nodiscard]] kb::Milliseconds budget() const noexcept {
    constexpr uint64_t kMax = std::numeric_limits<uint32_t>::max();
    uint64_t total = 0U;
    for (std::size_t i = 0U; i < stageCount(); ++i) {
      if (enabled(i)) {
        total += static_cast<uint64_t>(derived().StageDeadline(i).Count());
        total += static_cast<uint64_t>(pollInterval().Count());
      }
    }
    return kb::Milliseconds{static_cast<uint32_t>((total > kMax) ? kMax : total)};
  }

  [[nodiscard]] bool enabled(std::size_t index) const noexcept {
    static_cast<void>(index); // used only in one branch of the `if constexpr` below
    bool on = true;
    if constexpr (requires(const Derived& d) {
                    { d.StageEnabled(index) } noexcept -> std::same_as<bool>;
                  }) {
      on = (!Derived::kStartupPlan[index].optional) || derived().StageEnabled(index);
    }
    return on;
  }

  void beginRun(kb::Instant now) noexcept {
    running_ = true;
    stage_ = 0U;
    beginStage(now);
    report_value_ = StartupReport{};
    report_value_.runs = SaturatingIncrement(report_runs_);
    report_runs_ = report_value_.runs;
    report_value_.stages = static_cast<uint8_t>(stageCount());
    report_.Publish(report_value_);
    health_.BeginStart(recovering_);
    if (!driver_.has_value()) {
      derived().EmplaceDriver(driver_);
    }
  }

  void beginStage(kb::Instant now) noexcept {
    stage_started_ = now;
    stage_calls_ = 0U;
  }

  // One step of the current stage: skip disabled stages, run, check the deadline, advance.
  [[nodiscard]] kb::Result<StartProgress> step(kb::Instant now) noexcept {
    while ((stage_ < stageCount()) && (!enabled(stage_))) {
      ++stage_;
      beginStage(now);
    }
    kb::Result<StartProgress> result = kb::Result<StartProgress>::Success(StartProgress::Pending);
    if (stage_ >= stageCount()) {
      completeRun(now);
      result = kb::Result<StartProgress>::Success(StartProgress::Done);
    } else if (driver_.has_value()) {
      result = runCurrent(now, *driver_);
    } else {
      failRun(kb::Status::Fault, 0U, now);
      result = kb::Result<StartProgress>::Failure(kb::Status::Fault);
    }
    return result;
  }

  [[nodiscard]] kb::Result<StartProgress> runCurrent(kb::Instant now, Driver& driver) noexcept {
    const Stage& stage = Derived::kStartupPlan[stage_];
    StageCall call{};
    call.index = stage_;
    call.now = now;
    call.started = stage_started_;
    call.calls = stage_calls_;
    stage_calls_ = SaturatingIncrement(stage_calls_);
    kb::Result<StartProgress> outcome = derived().RunStage(call, driver);
    const bool late =
        kb::utils::Deadline::After(stage_started_, derived().StageDeadline(stage_)).Expired(now);
    kb::Result<StartProgress> result = kb::Result<StartProgress>::Success(StartProgress::Pending);
    if (!outcome.IsOk()) {
      failRun(outcome.Error(), outcome.Detail(), now);
      result = Forward<StartProgress>(outcome);
    } else if (outcome.Value() == StartProgress::Done) {
      noteStage(now); // from the stage's first step to the step that finished it
      ++stage_;
      beginStage(now);
      result = finish(now);
    } else if ((stage.mode == StageMode::Sync) || late) {
      const kb::Status why =
          (stage.mode == StageMode::Sync) ? kb::Status::Fault : kb::Status::Timeout;
      failRun(why, 0U, now);
      result = kb::Result<StartProgress>::Failure(why);
    } else {
      // Polled stage still in progress inside its deadline.
    }
    return result;
  }

  [[nodiscard]] kb::Result<StartProgress> finish(kb::Instant now) noexcept {
    kb::Result<StartProgress> result = kb::Result<StartProgress>::Success(StartProgress::Pending);
    while ((stage_ < stageCount()) && (!enabled(stage_))) {
      ++stage_;
    }
    if (stage_ >= stageCount()) {
      completeRun(now);
      result = kb::Result<StartProgress>::Success(StartProgress::Done);
    }
    return result;
  }

  void noteStage(kb::Instant now) noexcept {
    constexpr uint64_t kMax = std::numeric_limits<uint32_t>::max();
    const uint64_t us = kb::utils::ElapsedUs(now, stage_started_);
    report_value_.stage_us[stage_] = static_cast<uint32_t>((us > kMax) ? kMax : us);
    report_value_.stage = static_cast<uint8_t>(stage_);
    report_value_.kinds = Derived::kStartupPlan[stage_].kinds;
  }

  void completeRun(kb::Instant now) noexcept {
    running_ = false;
    report_value_.status = kb::Status::Ok;
    report_value_.complete = true;
    report_.Publish(report_value_);
    health_.StartDone(now, recovering_);
    recovering_ = false;
    attempts_ = 0U;
  }

  void failRun(kb::Status status, uint32_t detail, kb::Instant now) noexcept {
    // Callers fail inside the plan; the clamp only guards the index.
    const std::size_t at =
        (stage_ < stageCount()) ? stage_ : (stageCount() - 1U); // GCOVR_EXCL_BR_LINE
    report_value_.stage = static_cast<uint8_t>(at);
    report_value_.kinds = Derived::kStartupPlan[at].kinds;
    report_value_.status = status;
    report_value_.detail = detail;
    report_value_.complete = false;
    report_.Publish(report_value_);
    if (driver_.has_value()) {
      abort(*driver_);
      driver_.reset();
    }
    running_ = false;
    health_.StartFailed(status, detail, now, recovering_);
    if (recovering_) {
      attempts_ = SaturatingIncrement(attempts_);
      backoff_ = kb::utils::Deadline::After(now, backoffFor(attempts_));
    }
    recovering_ = false;
  }

  [[nodiscard]] kb::Milliseconds backoffFor(uint32_t attempts) const noexcept {
    const uint32_t cap = config_.recovery.max_backoff.Count();
    uint64_t wait = config_.recovery.backoff.Count();
    for (uint32_t i = 1U; i < attempts; ++i) {
      wait = (wait >= cap) ? wait : (wait * 2U);
    }
    return kb::Milliseconds{static_cast<uint32_t>((wait > cap) ? cap : wait)};
  }

  void record(kb::Status status, uint32_t detail, kb::Instant now) noexcept {
    if (status == kb::Status::Ok) {
      health_.RecordOk(now);
    } else if (!counts(status, detail)) {
      health_.RecordRefused(status, detail, now);
    } else {
      health_.RecordFailure(status, detail, fatal(status, detail), now);
    }
  }

  // A caller's mistake (InvalidArgument by default) says nothing about the chip: it is recorded as
  // the last error but does not move the health state or the failure count.
  [[nodiscard]] bool counts(kb::Status status, uint32_t detail) const noexcept {
    static_cast<void>(detail); // used only in one branch of the `if constexpr` below
    bool counted = status != kb::Status::InvalidArgument;
    if constexpr (requires(const Derived& d) {
                    { d.CountsAgainstHealth(status, detail) } noexcept -> std::same_as<bool>;
                  }) {
      counted = derived().CountsAgainstHealth(status, detail);
    }
    return counted;
  }

  // --- Optional hooks of the derived handler, with their defaults.
  [[nodiscard]] bool fatal(kb::Status status, uint32_t detail) const noexcept {
    static_cast<void>(status); // used only in one branch of the `if constexpr` below
    static_cast<void>(detail); // used only in one branch of the `if constexpr` below
    bool is_fatal = false;
    if constexpr (requires(const Derived& d) {
                    { d.IsFatal(status, detail) } noexcept -> std::same_as<bool>;
                  }) {
      is_fatal = derived().IsFatal(status, detail);
    }
    return is_fatal;
  }

  void abort(Driver& driver) noexcept {
    static_cast<void>(driver); // used only in one branch of the `if constexpr` below
    if constexpr (requires(Derived& d) {
                    { d.AbortStartup(driver) } noexcept;
                  }) {
      derived().AbortStartup(driver);
    }
  }

  [[nodiscard]] kb::Result<void> stopDriver(Driver& driver) noexcept {
    static_cast<void>(driver); // used only in one branch of the `if constexpr` below
    kb::Result<void> result = kb::Result<void>::Success();
    if constexpr (requires(Derived& d) {
                    { d.StopDriver(driver) } noexcept -> std::same_as<kb::Result<void>>;
                  }) {
      result = derived().StopDriver(driver);
    }
    return result;
  }

  [[nodiscard]] kb::Result<void> recoverDriver(Driver& driver, kb::Instant now) noexcept {
    static_cast<void>(now); // used only in one branch of the `if constexpr` below
    kb::Result<void> result = kb::Result<void>::Success();
    if constexpr (requires(Derived& d) {
                    { d.RecoverDriver(driver, now) } noexcept -> std::same_as<kb::Result<void>>;
                  }) {
      result = derived().RecoverDriver(driver, now);
    } else {
      result = stopDriver(driver);
    }
    return result;
  }

  [[nodiscard]] kb::Result<SnapshotT> measure(Driver& driver, kb::Instant now) noexcept {
    static_cast<void>(driver); // used only in one branch of the `if constexpr` below
    static_cast<void>(now);    // used only in one branch of the `if constexpr` below
    kb::Result<SnapshotT> result = kb::Result<SnapshotT>::Failure(kb::Status::Unsupported);
    if constexpr (requires(Derived& d) {
                    { d.Measure(driver, now) } noexcept -> std::same_as<kb::Result<SnapshotT>>;
                  }) {
      result = derived().Measure(driver, now);
    }
    return result;
  }

  [[nodiscard]] kb::Result<ReplyT> runCommand(Driver& driver, const CommandT& command,
                                              kb::Instant now) noexcept {
    static_cast<void>(driver);  // used only in one branch of the `if constexpr` below
    static_cast<void>(command); // used only in one branch of the `if constexpr` below
    static_cast<void>(now);     // used only in one branch of the `if constexpr` below
    kb::Result<ReplyT> result = kb::Result<ReplyT>::Failure(kb::Status::Unsupported);
    if constexpr (requires(Derived& d) {
                    { d.Run(driver, command, now) } noexcept -> std::same_as<kb::Result<ReplyT>>;
                  }) {
      result = derived().Run(driver, command, now);
    }
    return result;
  }

  HandlerConfig config_;
  std::optional<Driver> driver_{};
  HealthTracker health_;
  Seqlock<Stamped<SnapshotT>> snapshot_{};
  Seqlock<StartupReport> report_;
  LivenessBeat liveness_;
  StartupReport report_value_{};
  kb::utils::Deadline backoff_;
  kb::Instant stage_started_;
  std::size_t stage_{0U};
  uint32_t stage_calls_{0U};
  uint32_t report_runs_{0U};
  uint32_t attempts_{0U};
  bool running_{false};
  bool recovering_{false};
};

} // namespace kb::handlers

#endif // KB_HANDLERS_COMMON_HANDLER_CORE_HPP
