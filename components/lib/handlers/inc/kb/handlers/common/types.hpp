/**
 * @file types.hpp
 * @brief The vocabulary of kb::handlers: thread models, bus kinds, health states and records, the
 * start-up stage kinds and the start-up report (ADR-0009, ADR-0010).
 *
 * Every type here is a plain value at namespace scope (no type is nested in a class template), so
 * the handler core, the facades, the bindings and the HAL all name the same types.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_HANDLERS_COMMON_TYPES_HPP
#define KB_HANDLERS_COMMON_TYPES_HPP

#include <array>
#include <cstddef>
#include <cstdint>

#include "kb/utils/status.hpp"
#include "kb/utils/time.hpp"

namespace kb::handlers {

/** @brief How the handlers of one bus are reached from several tasks (ADR-0009). */
enum class ThreadModel : uint8_t {
  Owner = 0,   ///< Model O: one owner task services every handler of the bus; no bus mutex.
  Guarded = 1, ///< Model G: each call takes the handler's mutex, then the bus mutex, both bounded.
};

/** @brief The kind of bus a handler's chip sits on; a BusDomain checks its bus against it. */
enum class BusKind : uint8_t {
  I2c = 0,  ///< kb::iface::I2cBus.
  Spi = 1,  ///< kb::iface::SpiDevice (one domain per physical bus; devices are per chip select).
  Uart = 2, ///< kb::iface::UartPort.
  Adc = 3,  ///< kb::iface::AdcChannel.
  Can = 4,  ///< kb::iface::CanBus.
  Pio = 5,  ///< A pulse or timing peripheral driven by a port (no kb contract; any bus type).
};

/** @brief The health of a handler. Only the start-up runner, `Recover()` and recorded calls change
 * it. */
enum class HealthState : uint8_t {
  Unknown = 0,    ///< Constructed, never started (or deinitialised): calls return NotReady.
  Starting = 1,   ///< The start-up plan is running.
  Ready = 2,      ///< Started; the last calls succeeded.
  Degraded = 3,   ///< Started; recent calls failed, fewer than the fault threshold in a row.
  Faulted = 4,    ///< A start failed, the threshold was reached or a fatal error was seen; calls
                  ///< return NotReady until `Recover()` or a new start succeeds.
  Recovering = 5, ///< `Recover()` is running the plan's recovery and start-up again.
};

namespace detail {
/// The names of the HealthState values, indexed by value.
inline constexpr std::array<const char*, 6U> kHealthStateNames{
    "Unknown", "Starting", "Ready", "Degraded", "Faulted", "Recovering"};
} // namespace detail

/** @brief The name of a health state, for logs; "Unknown" for a value that is no enumerator. */
[[nodiscard]] constexpr const char* ToString(HealthState state) noexcept {
  const auto index = static_cast<std::size_t>(state);
  return (index < detail::kHealthStateNames.size()) ? detail::kHealthStateNames[index] : "Unknown";
}

/** @brief True for the states in which a handler serves calls (Ready, Degraded). */
[[nodiscard]] constexpr bool IsServing(HealthState state) noexcept {
  return (state == HealthState::Ready) || (state == HealthState::Degraded);
}

/**
 * @brief The stage kinds of a start-up plan, as bits: a stage may cover several kinds (one driver
 * call), and a plan lists its stages in this order (ADR-0010).
 */
namespace stage {
inline constexpr uint16_t kPower = 0x0001U;     ///< Enable a rail or an enable pin, settle.
inline constexpr uint16_t kHostReset = 0x0002U; ///< Host-driven reset: pulse, hold, release.
inline constexpr uint16_t kBoot = 0x0004U;      ///< Boot time or a ready condition.
inline constexpr uint16_t kLink = 0x0008U;      ///< Bring up a transport session.
inline constexpr uint16_t kIdentify = 0x0010U;  ///< Read the ID, select an applet, probe.
inline constexpr uint16_t kSecure = 0x0020U;    ///< Open a secure channel needed for normal use.
inline constexpr uint16_t kConfigure = 0x0040U; ///< Apply the configuration.
inline constexpr uint16_t kVerify = 0x0080U;    ///< Read back, check the lifecycle state.
inline constexpr uint16_t kAll = 0x00FFU;       ///< Every kind.
} // namespace stage

/** @brief Whether a stage is one call or a call repeated until it reports done. */
enum class StageMode : uint8_t {
  Sync = 0,   ///< One call; it returns done or a failure (Pending is a failure: Fault).
  Polled = 1, ///< Called once per step until done, a failure, or its deadline.
};

/**
 * @brief One declared stage of a start-up plan. The binding supplies the call, the deadline (from
 * its configuration) and, for an optional stage, whether the HAL supplied what it needs.
 */
struct Stage {
  uint16_t kinds{0U};              ///< One or more `stage::k*` bits.
  StageMode mode{StageMode::Sync}; ///< Sync or polled.
  bool optional{false};            ///< Skipped when the binding reports it not enabled.
};

/** @brief The most stages a plan may declare (one per kind). */
inline constexpr std::size_t kMaxStages = 8U;

/** @brief The lowest kind bit of `kinds` (0 for none). */
[[nodiscard]] constexpr uint16_t LowestKind(uint16_t kinds) noexcept {
  uint16_t bit = 1U;
  uint16_t found = 0U;
  for (std::size_t i = 0U; i < kMaxStages; ++i) {
    if ((found == 0U) && ((kinds & bit) != 0U)) {
      found = bit;
    }
    bit = static_cast<uint16_t>(bit << 1U);
  }
  return found;
}

/** @brief The highest kind bit of `kinds` (0 for none). */
[[nodiscard]] constexpr uint16_t HighestKind(uint16_t kinds) noexcept {
  uint16_t bit = 1U;
  uint16_t found = 0U;
  for (std::size_t i = 0U; i < kMaxStages; ++i) {
    if ((kinds & bit) != 0U) {
      found = bit;
    }
    bit = static_cast<uint16_t>(bit << 1U);
  }
  return found;
}

/**
 * @brief True for a well-formed plan: 1 to kMaxStages stages, each with known kinds, every stage
 * strictly after the one before it in kind order (no kind twice). Checked by `static_assert` in
 * HandlerCore, so a malformed `kStartupPlan` does not compile.
 */
template <std::size_t N>
[[nodiscard]] constexpr bool IsValidPlan(const std::array<Stage, N>& plan) noexcept {
  bool ok = (N >= 1U) && (N <= kMaxStages);
  uint16_t previous = 0U;
  for (const Stage& s : plan) {
    const bool known = (s.kinds != 0U) && ((s.kinds & static_cast<uint16_t>(~stage::kAll)) == 0U);
    const bool ordered = known && (LowestKind(s.kinds) > previous);
    ok = ok && ordered;
    previous = known ? HighestKind(s.kinds) : previous;
  }
  return ok;
}

/** @brief What one start-up step reached. */
enum class StartProgress : uint8_t {
  Pending = 0, ///< The current stage needs another step (polled stage, or the next stage).
  Done = 1,    ///< The last stage succeeded; the handler is Ready.
};

/**
 * @brief What the last start-up run reached (ADR-0010): the stage, its kinds, the outcome with the
 * driver's own error in `detail`, and the time each stage took.
 */
struct StartupReport {
  std::array<uint32_t, kMaxStages> stage_us{}; ///< Microseconds per stage (saturating).
  uint32_t detail{0U};                         ///< The driver's error code of the failure; 0.
  uint32_t runs{0U};                           ///< Start-up runs begun since construction.
  uint16_t kinds{0U};                          ///< Kinds of the stage reached.
  uint8_t stage{0U};                           ///< Index of the stage reached (failed or last).
  uint8_t stages{0U};                          ///< Stages in the plan.
  kb::Status status{kb::Status::NotReady};     ///< Ok after a complete start.
  bool complete{false};                        ///< True after the last stage succeeded.
};

/** @brief When health changes state. Board values: the HAL sets them in the handler config. */
struct HealthPolicy {
  uint32_t fault_after{3U}; ///< Consecutive failures that make a serving handler Faulted (>= 1).
};

/** @brief How often `Recover()` may run: exponential backoff between attempts, then a limit. */
struct RecoveryPolicy {
  kb::Milliseconds backoff{10U};       ///< Wait after the first failed attempt.
  kb::Milliseconds max_backoff{1000U}; ///< The wait never grows past this.
  uint32_t max_attempts{0U}; ///< Failed attempts in a row before Recover() refuses; 0 = no limit.
};

/**
 * @brief The health record of one handler, published lock-free (a seqlock copy): the state, the
 * counters, the last failure with the driver's own code, and the last good time.
 */
struct HealthRecord {
  uint64_t last_error_us{0U};     ///< When the last failure was recorded (kb::rtos::Now()).
  uint64_t last_good_us{0U};      ///< When the last success was recorded; valid when `has_good`.
  uint32_t ok_calls{0U};          ///< Calls that succeeded (start-up included).
  uint32_t failed_calls{0U};      ///< Calls that failed (start-up included).
  uint32_t refused_calls{0U};     ///< Calls refused for the caller's mistake (not a chip failure).
  uint32_t consecutive{0U};       ///< Failures in a row since the last success.
  uint32_t starts{0U};            ///< Completed start-ups.
  uint32_t start_failures{0U};    ///< Start-up runs that failed.
  uint32_t recoveries{0U};        ///< Recover() runs that completed.
  uint32_t recovery_failures{0U}; ///< Recover() runs that failed.
  uint32_t last_detail{0U};       ///< The driver's error code of the last failure (vendor detail).
  kb::Status last_status{kb::Status::Ok};  ///< The kb::Status of the last failure; Ok when none.
  HealthState state{HealthState::Unknown}; ///< Where the handler is.
  bool has_good{false};                    ///< True once a call succeeded.
  bool has_error{false};                   ///< True once a call failed.
};

} // namespace kb::handlers

#endif // KB_HANDLERS_COMMON_TYPES_HPP
