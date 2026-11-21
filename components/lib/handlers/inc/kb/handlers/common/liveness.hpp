/**
 * @file liveness.hpp
 * @brief The liveness feed of handlers for a watchdog monitor: each handler beats when it is
 * serviced (model O) or called (model G); a `LivenessMonitor` sees which ones stopped beating.
 *
 * A beat is a counter and the instant of the last beat, both atomics: the handler's context writes,
 * the monitor reads from its own task. The monitor never calls a handler and never blocks; what it
 * does with a stale handler (kick or starve the hardware watchdog, report, recover) is the HAL's
 * decision.
 *
 * A context that stops on purpose (a parked thread, a handler stopped for a power state) declares
 * it with `Park()`: a parked beat is never stale, and its window starts again when it beats or
 * `Resume()`s. `Verdict(now)` is what the watchdog owner reads before it kicks: kick only when at
 * least one beat is watched and none is stale.
 *
 * @code
 * kb::handlers::LivenessMonitor<4U> monitor{};
 * static_cast<void>(monitor.Watch(&imu.Liveness(), kb::Milliseconds{50U}));
 * const kb::handlers::LivenessVerdict v = monitor.Verdict(kb::rtos::Now());
 * if (v.kick) { watchdog.Kick(); }  // v.stale: bit i set, watch i is late
 * @endcode
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_HANDLERS_COMMON_LIVENESS_HPP
#define KB_HANDLERS_COMMON_LIVENESS_HPP

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "kb/utils/result.hpp"
#include "kb/utils/status.hpp"
#include "kb/utils/time.hpp"

namespace kb::handlers {

/** @brief One handler's beat: a count and the instant of the last beat. */
class LivenessBeat {
public:
  constexpr LivenessBeat() noexcept = default;
  LivenessBeat(const LivenessBeat&) = delete;
  LivenessBeat& operator=(const LivenessBeat&) = delete;
  LivenessBeat(LivenessBeat&&) = delete;
  LivenessBeat& operator=(LivenessBeat&&) = delete;
  ~LivenessBeat() = default;

  /** @brief Records a beat at `now` (the handler's context only); a parked beat resumes. */
  void Beat(kb::Instant now) noexcept {
    last_us_.store(now.Us(), std::memory_order_relaxed);
    parked_.store(false, std::memory_order_relaxed);
    static_cast<void>(beats_.fetch_add(1U, std::memory_order_release));
  }

  /** @brief Declares that this context stops beating on purpose (never stale while parked). */
  void Park() noexcept {
    parked_.store(true, std::memory_order_release);
  }

  /** @brief Ends a Park(); the monitor's window starts again at its next check. */
  void Resume() noexcept {
    static_cast<void>(beats_.fetch_add(1U, std::memory_order_relaxed));
    parked_.store(false, std::memory_order_release);
  }

  /** @brief True while parked. */
  [[nodiscard]] bool Parked() const noexcept {
    return parked_.load(std::memory_order_acquire);
  }

  /** @brief Beats so far (wraps). */
  [[nodiscard]] uint32_t Beats() const noexcept {
    return beats_.load(std::memory_order_acquire);
  }

  /** @brief The instant of the last beat; 0 before the first. */
  [[nodiscard]] kb::Instant LastBeat() const noexcept {
    return kb::Instant{last_us_.load(std::memory_order_relaxed)};
  }

private:
  std::atomic<uint64_t> last_us_{0U};
  std::atomic<uint32_t> beats_{0U};
  std::atomic<bool> parked_{false};
};

/** @brief What the watchdog owner reads before it kicks (LivenessMonitor::Verdict). */
struct LivenessVerdict {
  uint32_t stale{0U};   ///< Bit i set: watch i made no progress within its window.
  uint32_t parked{0U};  ///< Bit i set: watch i is parked (not stale by declaration).
  uint32_t watched{0U}; ///< Watches in use.
  bool kick{false};     ///< True when at least one beat is watched and none is stale.
};

/** @brief One watched beat of a LivenessMonitor. */
struct LivenessWatch {
  const LivenessBeat* beat{nullptr}; ///< The watched beat; null marks a free slot.
  uint64_t window_us{0U};            ///< The longest gap between beats that is still alive.
  uint32_t seen{0U};                 ///< The beat count at the last progress.
  uint64_t since_us{0U};             ///< When progress was last seen.
  bool started{false};               ///< True after the first Check() of this watch.
};

/**
 * @brief Watches up to N beats; `Check(now)` gives a bit per watch that made no progress within its
 * window. Used from one monitor task.
 * @tparam N Watches, 1 to 32.
 */
template <std::size_t N>
  requires((N >= 1U) && (N <= 32U))
class LivenessMonitor {
public:
  constexpr LivenessMonitor() noexcept = default;

  /**
   * @brief Watches `beat` (a handler's Liveness(), which outlives the monitor), alive while it
   * beats at least once per `window`.
   * @return The watch index; `Full` when N watches are in use, `InvalidArgument` for a null beat or
   * a zero window.
   */
  [[nodiscard]] kb::Result<uint32_t> Watch(const LivenessBeat* beat,
                                           kb::Milliseconds window) noexcept {
    kb::Result<uint32_t> result = kb::Result<uint32_t>::Failure(kb::Status::Full);
    if ((beat == nullptr) || (window.Count() == 0U)) {
      result = kb::Result<uint32_t>::Failure(kb::Status::InvalidArgument);
    } else {
      bool placed = false;
      for (std::size_t i = 0U; i < N; ++i) {
        if ((!placed) && (watches_[i].beat == nullptr)) {
          watches_[i] = LivenessWatch{};
          watches_[i].beat = beat;
          watches_[i].window_us = static_cast<uint64_t>(window.Count()) * kUsPerMs;
          placed = true;
          result = kb::Result<uint32_t>::Success(static_cast<uint32_t>(i));
        }
      }
    }
    return result;
  }

  /**
   * @brief Checks every watch at `now`.
   * @return Bit i set when watch i made no progress for longer than its window.
   */
  [[nodiscard]] uint32_t Check(kb::Instant now) noexcept {
    uint32_t stale = 0U;
    for (std::size_t i = 0U; i < N; ++i) {
      LivenessWatch& w = watches_[i];
      if (w.beat != nullptr) {
        const uint32_t beats = w.beat->Beats();
        if (w.beat->Parked()) {
          w.started = false; // the window starts again after the park
        } else if ((!w.started) || (beats != w.seen)) {
          w.started = true;
          w.seen = beats;
          w.since_us = now.Us();
        } else if (kb::utils::ElapsedUs(now, kb::Instant{w.since_us}) > w.window_us) {
          stale |= (1UL << static_cast<uint32_t>(i));
        } else {
          // Alive, no progress yet within the window.
        }
      }
    }
    return stale;
  }

  /**
   * @brief Checks every watch at `now` (as Check()) and says whether the watchdog may be kicked.
   */
  [[nodiscard]] LivenessVerdict Verdict(kb::Instant now) noexcept {
    LivenessVerdict v{};
    v.stale = Check(now);
    for (std::size_t i = 0U; i < N; ++i) {
      const LivenessWatch& w = watches_[i];
      if (w.beat != nullptr) {
        ++v.watched;
        if (w.beat->Parked()) {
          v.parked |= (1UL << static_cast<uint32_t>(i));
        }
      }
    }
    v.kick = (v.watched != 0U) && (v.stale == 0U);
    return v;
  }

private:
  static constexpr uint64_t kUsPerMs = 1000U;
  std::array<LivenessWatch, N> watches_{};
};

} // namespace kb::handlers

#endif // KB_HANDLERS_COMMON_LIVENESS_HPP
