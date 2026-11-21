// Toy handlers with scripted start-up stages, small bus fakes and a periodic test timer, shared by
// the unit tests of kb::handlers (components/lib/handlers/tests).
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#ifndef KB_HANDLERS_TESTS_HANDLER_TEST_SUPPORT_HPP
#define KB_HANDLERS_TESTS_HANDLER_TEST_SUPPORT_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

#include "kb/handlers/handlers.hpp"
#include "kb/interfaces/host/host.hpp"
#include "kb/rtos/timer.hpp"

namespace kb::handlers {

/// Makes the ServiceKey that the tests of the single-threaded core pass (ServiceKey befriends this
/// peer).
struct ServiceKeyTestPeer {
  [[nodiscard]] static ServiceKey Make() noexcept {
    return ServiceKey{};
  }
};

} // namespace kb::handlers

namespace kbt {

namespace h = kb::handlers;

inline constexpr uint32_t kAlways = std::numeric_limits<uint32_t>::max();

/// The snapshot of the toy handlers.
struct Reading {
  uint32_t value{0U};
};

/// The command of the toy handlers.
struct Command {
  uint32_t op{0U};
  uint32_t arg{0U};
};

/// The reply of the toy handlers.
struct Reply {
  uint32_t value{0U};
};

/// What one stage of a scripted plan answers.
struct StageScript {
  uint32_t pending{0U}; ///< Pending answers (by StageCall::calls) before the stage is done.
  kb::Status fail{kb::Status::Ok}; ///< The failure given once the pending steps are spent.
  uint32_t fail_times{0U};         ///< How many runs fail (kAlways: every run).
  uint32_t detail{0U};             ///< The driver detail of the failure.
  uint32_t deadline_ms{100U};      ///< StageDeadline().
  uint32_t elapse_ms{0U};          ///< Fake time each call takes (through Script::elapse).
  bool enabled{true};              ///< StageEnabled().
  uint32_t calls{0U};              ///< RunStage() calls seen.
  h::StageCall last{};             ///< The last call.
};

class ToyDriver;

/// The scripted behaviour and the observed calls of one toy handler.
struct Script {
  std::array<StageScript, h::kMaxStages> stages{};
  /// Decides a stage instead of `stages` when set.
  kb::Result<h::StartProgress> (*on_stage)(Script& script,
                                           const h::StageCall& call) noexcept {nullptr};
  /// Answers a command instead of run_status when set.
  kb::Result<Reply> (*on_run)(Script& script, const Command& command) noexcept {nullptr};
  /// Lets fake time pass inside a stage (the none backend's AdvanceTicks).
  void (*elapse)(uint32_t ms) noexcept {nullptr};
  void* user{nullptr};                      ///< Context of the hooks.
  std::optional<ToyDriver>* stash{nullptr}; ///< The driver slot EmplaceDriver() was given.
  bool emplace{true};                       ///< False: EmplaceDriver() leaves the slot empty.
  uint32_t emplace_calls{0U};
  uint32_t emplaced{0U};
  uint32_t destroyed{0U};
  uint32_t aborts{0U};
  uint32_t stops{0U};
  uint32_t recovers{0U};
  uint32_t measures{0U};
  uint32_t runs{0U};
  kb::Status stop_status{kb::Status::Ok};
  kb::Status recover_status{kb::Status::Ok};
  kb::Status measure_status{kb::Status::Ok};
  uint32_t measure_detail{0U};
  uint32_t measure_value{0U};
  kb::Status run_status{kb::Status::Ok};
  uint32_t run_detail{0U};
  Command last_command{};
  kb::Status fatal_status{kb::Status::Ok};   ///< IsFatal() is true for this status.
  kb::Status refused_status{kb::Status::Ok}; ///< CountsAgainstHealth() is false for this status.
};

/// The driver of the toy handlers: counts its constructions and destructions.
class ToyDriver {
public:
  explicit ToyDriver(Script& script) noexcept : script_(script) {
    ++script_.emplaced;
  }
  ~ToyDriver() {
    ++script_.destroyed;
  }
  ToyDriver(const ToyDriver&) = delete;
  ToyDriver& operator=(const ToyDriver&) = delete;
  ToyDriver(ToyDriver&&) = delete;
  ToyDriver& operator=(ToyDriver&&) = delete;

private:
  Script& script_;
};

/// The optional hooks a toy handler declares.
namespace feat {
inline constexpr uint32_t kStageEnabled = 0x01U;
inline constexpr uint32_t kAbort = 0x02U;
inline constexpr uint32_t kStop = 0x04U;
inline constexpr uint32_t kRecover = 0x08U;
inline constexpr uint32_t kFatal = 0x10U;
inline constexpr uint32_t kCounts = 0x20U;
inline constexpr uint32_t kMeasure = 0x40U;
inline constexpr uint32_t kRun = 0x80U;
inline constexpr uint32_t kAll = 0xFFU;
inline constexpr uint32_t kNone = 0x00U;
} // namespace feat

/// One sync stage covering Identify and Configure (the generic plan).
struct PlanSync {
  static constexpr std::array<h::Stage, 1> kPlan{
      {{.kinds = static_cast<uint16_t>(h::stage::kIdentify | h::stage::kConfigure),
        .mode = h::StageMode::Sync,
        .optional = false}}};
};

/// Optional Power (sync), Boot + Link (polled), Configure (sync).
struct PlanThree {
  static constexpr std::array<h::Stage, 3> kPlan{
      {{.kinds = h::stage::kPower, .mode = h::StageMode::Sync, .optional = true},
       {.kinds = static_cast<uint16_t>(h::stage::kBoot | h::stage::kLink),
        .mode = h::StageMode::Polled,
        .optional = false},
       {.kinds = h::stage::kConfigure, .mode = h::StageMode::Sync, .optional = false}}};
};

/// Identify (sync) and two optional trailing stages.
struct PlanTrailing {
  static constexpr std::array<h::Stage, 3> kPlan{
      {{.kinds = h::stage::kIdentify, .mode = h::StageMode::Sync, .optional = false},
       {.kinds = h::stage::kConfigure, .mode = h::StageMode::Sync, .optional = true},
       {.kinds = h::stage::kVerify, .mode = h::StageMode::Sync, .optional = true}}};
};

/// Optional stages before and between required ones.
struct PlanMiddle {
  static constexpr std::array<h::Stage, 4> kPlan{
      {{.kinds = h::stage::kPower, .mode = h::StageMode::Sync, .optional = true},
       {.kinds = h::stage::kIdentify, .mode = h::StageMode::Sync, .optional = false},
       {.kinds = h::stage::kConfigure, .mode = h::StageMode::Sync, .optional = true},
       {.kinds = h::stage::kVerify, .mode = h::StageMode::Sync, .optional = false}}};
};

/// Only optional stages.
struct PlanOptionalOnly {
  static constexpr std::array<h::Stage, 2> kPlan{
      {{.kinds = h::stage::kPower, .mode = h::StageMode::Sync, .optional = true},
       {.kinds = h::stage::kHostReset, .mode = h::StageMode::Sync, .optional = true}}};
};

/// One polled stage.
struct PlanPolled {
  static constexpr std::array<h::Stage, 1> kPlan{
      {{.kinds = h::stage::kBoot, .mode = h::StageMode::Polled, .optional = false}}};
};

/// A sync stage, then a polled one.
struct PlanSyncPolled {
  static constexpr std::array<h::Stage, 2> kPlan{
      {{.kinds = h::stage::kIdentify, .mode = h::StageMode::Sync, .optional = false},
       {.kinds = h::stage::kConfigure, .mode = h::StageMode::Polled, .optional = false}}};
};

/// Two sync stages.
struct PlanTwoSync {
  static constexpr std::array<h::Stage, 2> kPlan{
      {{.kinds = h::stage::kIdentify, .mode = h::StageMode::Sync, .optional = false},
       {.kinds = h::stage::kConfigure, .mode = h::StageMode::Sync, .optional = false}}};
};

/// The scripted answer of one stage call.
[[nodiscard]] inline kb::Result<h::StartProgress> ScriptedStage(Script& script,
                                                                const h::StageCall& call) noexcept {
  StageScript& s = script.stages[call.index];
  kb::Result<h::StartProgress> result =
      kb::Result<h::StartProgress>::Success(h::StartProgress::Done);
  if (call.calls < s.pending) {
    result = kb::Result<h::StartProgress>::Success(h::StartProgress::Pending);
  } else if ((s.fail != kb::Status::Ok) && (s.fail_times != 0U)) {
    if (s.fail_times != kAlways) {
      --s.fail_times;
    }
    result = kb::Result<h::StartProgress>::Failure(s.fail, s.detail);
  } else {
    // Done.
  }
  return result;
}

/**
 * A handler over ToyDriver whose stages and calls follow a Script.
 * @tparam Plan A plan struct (kPlan).
 * @tparam F The optional hooks it declares (feat bits).
 * @tparam RequiresOwner Its kRequiresOwner.
 */
template <typename Plan, uint32_t F = feat::kAll, bool RequiresOwner = false>
class Toy : public h::HandlerCore<Toy<Plan, F, RequiresOwner>, ToyDriver, Reading, Command, Reply> {
  using Core = h::HandlerCore<Toy<Plan, F, RequiresOwner>, ToyDriver, Reading, Command, Reply>;

public:
  static constexpr auto kStartupPlan = Plan::kPlan;
  static constexpr bool kRequiresOwner = RequiresOwner;
  static constexpr h::BusKind kBus = h::BusKind::I2c;

  Toy(Script& script, const h::HandlerConfig& config) noexcept : Core(config), script_(script) {}

  void EmplaceDriver(std::optional<ToyDriver>& slot) noexcept {
    ++script_.emplace_calls;
    script_.stash = &slot;
    if (script_.emplace) {
      slot.emplace(script_);
    }
  }

  [[nodiscard]] kb::Result<h::StartProgress> RunStage(const h::StageCall& call,
                                                      ToyDriver& driver) noexcept {
    static_cast<void>(driver);
    StageScript& s = script_.stages[call.index];
    ++s.calls;
    s.last = call;
    if ((s.elapse_ms != 0U) && (script_.elapse != nullptr)) {
      script_.elapse(s.elapse_ms);
    }
    return (script_.on_stage != nullptr) ? script_.on_stage(script_, call)
                                         : ScriptedStage(script_, call);
  }

  [[nodiscard]] kb::Milliseconds StageDeadline(std::size_t index) const noexcept {
    return kb::Milliseconds{script_.stages[index].deadline_ms};
  }

  [[nodiscard]] bool StageEnabled(std::size_t index) const noexcept
    requires((F & feat::kStageEnabled) != 0U)
  {
    return script_.stages[index].enabled;
  }

  void AbortStartup(ToyDriver& driver) noexcept
    requires((F & feat::kAbort) != 0U)
  {
    static_cast<void>(driver);
    ++script_.aborts;
  }

  [[nodiscard]] kb::Result<void> StopDriver(ToyDriver& driver) noexcept
    requires((F & feat::kStop) != 0U)
  {
    static_cast<void>(driver);
    ++script_.stops;
    return kb::Result<void>::FromStatus(script_.stop_status);
  }

  [[nodiscard]] kb::Result<void> RecoverDriver(ToyDriver& driver, kb::Instant now) noexcept
    requires((F & feat::kRecover) != 0U)
  {
    static_cast<void>(driver);
    static_cast<void>(now);
    ++script_.recovers;
    return kb::Result<void>::FromStatus(script_.recover_status);
  }

  [[nodiscard]] bool IsFatal(kb::Status status, uint32_t detail) const noexcept
    requires((F & feat::kFatal) != 0U)
  {
    static_cast<void>(detail);
    return status == script_.fatal_status;
  }

  [[nodiscard]] bool CountsAgainstHealth(kb::Status status, uint32_t detail) const noexcept
    requires((F & feat::kCounts) != 0U)
  {
    static_cast<void>(detail);
    return status != script_.refused_status;
  }

  [[nodiscard]] kb::Result<Reading> Measure(ToyDriver& driver, kb::Instant now) noexcept
    requires((F & feat::kMeasure) != 0U)
  {
    static_cast<void>(driver);
    static_cast<void>(now);
    ++script_.measures;
    return (script_.measure_status == kb::Status::Ok)
               ? kb::Result<Reading>::Success(Reading{script_.measure_value})
               : kb::Result<Reading>::Failure(script_.measure_status, script_.measure_detail);
  }

  [[nodiscard]] kb::Result<Reply> Run(ToyDriver& driver, const Command& command,
                                      kb::Instant now) noexcept
    requires((F & feat::kRun) != 0U)
  {
    static_cast<void>(driver);
    static_cast<void>(now);
    ++script_.runs;
    script_.last_command = command;
    kb::Result<Reply> result = kb::Result<Reply>::Success(Reply{command.arg + 1U});
    if (script_.on_run != nullptr) {
      result = script_.on_run(script_, command);
    } else if (script_.run_status != kb::Status::Ok) {
      result = kb::Result<Reply>::Failure(script_.run_status, script_.run_detail);
    } else {
      // Success.
    }
    return result;
  }

  /// The core's driver pointer (protected there).
  [[nodiscard]] ToyDriver* Driver() noexcept {
    return this->DriverPointer();
  }

private:
  Script& script_;
};

/// A bus tag of kind I2C whose controller can free a stuck bus.
struct RecoverableI2c {
  static constexpr h::BusKind kKind = h::BusKind::I2c;
  [[nodiscard]] kb::Status RecoverBus() noexcept {
    ++calls;
    return answer;
  }
  kb::Status answer{kb::Status::Ok};
  uint32_t calls{0U};
};

/// A periodic 1 ms timer that calls the function set in TickerHook() (none backend: it fires while
/// time advances, so it can play another task inside a bounded wait).
struct TickerHookSlot {
  void (*fn)(void* context) noexcept {nullptr};
  void* context{nullptr};
};

[[nodiscard]] inline TickerHookSlot& TickerHook() noexcept {
  static TickerHookSlot slot{};
  return slot;
}

inline void TickerFire(void* context) noexcept {
  static_cast<void>(context);
  const TickerHookSlot& slot = TickerHook();
  if (slot.fn != nullptr) {
    slot.fn(slot.context);
  }
}

[[nodiscard]] inline kb::rtos::Timer& Ticker() noexcept {
  static kb::rtos::Timer timer{};
  static const bool kCreated =
      timer.Create(&TickerFire, nullptr, kb::Milliseconds{1U}, true).IsOk();
  static_cast<void>(kCreated);
  return timer;
}

/// Runs `fn(context)` every millisecond of fake time while it lives.
class TickerScope {
public:
  explicit TickerScope(void (*fn)(void* context) noexcept, void* context) noexcept {
    TickerHook().fn = fn;
    TickerHook().context = context;
    static_cast<void>(Ticker().Start());
  }
  ~TickerScope() {
    static_cast<void>(Ticker().Stop());
    TickerHook().fn = nullptr;
    TickerHook().context = nullptr;
  }
  TickerScope(const TickerScope&) = delete;
  TickerScope& operator=(const TickerScope&) = delete;
  TickerScope(TickerScope&&) = delete;
  TickerScope& operator=(TickerScope&&) = delete;
};

/// `t` plus `us` microseconds.
[[nodiscard]] constexpr kb::Instant Plus(kb::Instant t, uint64_t us) noexcept {
  return kb::Instant{t.Us() + us};
}

} // namespace kbt

#endif // KB_HANDLERS_TESTS_HANDLER_TEST_SUPPORT_HPP
