/**
 * @file thread.hpp
 * @brief kb::rtos::Thread<Derived, StackBytes>: the CRTP thread base (ADR-0006 register) with a
 * static stack, a fixed-rate run loop, overrun counting, stop and park handling, a verified start
 * and step and stack statistics.
 *
 * A derived thread supplies three hooks, public or reachable by friendship:
 *
 * | Hook | Called | Returns |
 * | --- | --- | --- |
 * | `bool Setup() noexcept` | once, on the new thread | false fails the start: the thread ends
 * `Failed` | | `bool Step() noexcept` | every period (or back to back with a zero period) | false
 * ends the loop | | `void Cleanup() noexcept` | once, last, also after a failed Setup | - |
 *
 * @code
 * class Blinker : public kb::rtos::Thread<Blinker, 2048U> {
 * public:
 *   bool Setup() noexcept { return true; }
 *   bool Step() noexcept { led_.Toggle(); return true; }
 *   void Cleanup() noexcept {}
 * };
 * Blinker blinker{};                                     // static, constexpr-constructed, no RTOS
 * call kb::rtos::ThreadConfig cfg{}; cfg.name = "blink"; cfg.period = kb::Milliseconds{500U}; if
 * (!blinker.Start(cfg).IsOk()) { ... }                // after the scheduler runs
 * @endcode
 *
 * The constructor never touches the RTOS; `Start()` creates the thread and fails closed
 * (`AlreadyStarted`, `InvalidArgument`, or the backend's status); `StartAndWaitToVerify(config,
 * timeout)` also waits until Setup() has returned and fails closed when it failed (`Fault`) or did
 * not return in time (`Timeout`). `RequestStop()` ends the loop after the current step;
 * `Join(timeout)` waits for the end. `RequestPark()` parks the loop after the current step until
 * `Resume()` (or `ResumeFromIsr()` from an interrupt handler); the period grid restarts on resume,
 * so a long park is not counted as overruns. `StackFreeBytes()` is the stack's high-water mark
 * where the backend measures it. No virtual function is involved: hooks are called through
 * `Derived`.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_RTOS_THREAD_HPP
#define KB_RTOS_THREAD_HPP

#include <algorithm>
#include <array>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "kb/rtos/backend.hpp"
#include "kb/rtos/backend_rules.hpp"
#include "kb/rtos/semaphore.hpp"
#include "kb/rtos/time.hpp"
#include "kb/utils/result.hpp"
#include "kb/utils/time.hpp"

namespace kb::rtos {

/** @brief Where a thread is in its life. */
enum class ThreadState : uint8_t {
  Idle = 0,     ///< Constructed, not started.
  Starting = 1, ///< Created; Setup() has not returned yet.
  Running = 2,  ///< In the step loop.
  Stopped =
      3,      ///< The loop ended (Step() returned false or a stop was requested) and Cleanup() ran.
  Failed = 4, ///< Setup() returned false; Cleanup() ran.
};

/** @brief How a thread starts. */
struct ThreadConfig {
  const char* name{"kb"}; ///< Name for the RTOS's diagnostics.
  uint32_t priority{1U};  ///< Backend priority; higher runs first.
  Milliseconds period{
      0U}; ///< Step period on a fixed grid; 0 runs steps back to back (yielding between).
};

/** @brief Run-time statistics of a thread, read with Thread::Stats(). */
struct ThreadStats {
  uint32_t steps{0U};       ///< Steps completed.
  uint32_t overruns{0U};    ///< Periods whose wake-up had already passed.
  uint32_t max_step_us{0U}; ///< Longest step, microseconds (saturates).
};

/** @brief The hooks a Thread's Derived class must provide. */
template <typename D>
concept ThreadBody = requires(D& d) {
  { d.Setup() } noexcept -> std::same_as<bool>;
  { d.Step() } noexcept -> std::same_as<bool>;
  { d.Cleanup() } noexcept;
};

/**
 * @brief The CRTP thread base.
 * @tparam Derived The thread class (ThreadBody).
 * @tparam StackBytes Static stack size in bytes; at least 1024.
 */
template <typename Derived, std::size_t StackBytes>
  requires(StackBytes >= 1024U)
class Thread : private detail::ThreadCore {
public:
  // Public, so a derived thread can be brace-initialised as an aggregate (`Blinker blinker{};`).
  constexpr Thread() noexcept : detail::ThreadCore{&Thread::runCore} {}
  /** @brief Releases the backend's hold on an ended thread (joins it on the host backend). */
  ~Thread() {
    if (State() != ThreadState::Idle) {
      backend::ThreadRelease(storage_);
    }
  }
  Thread(const Thread&) = delete;
  Thread& operator=(const Thread&) = delete;
  Thread(Thread&&) = delete;
  Thread& operator=(Thread&&) = delete;

  /** @brief Creates the thread with `config`. Fails closed; a second call gives AlreadyStarted. */
  [[nodiscard]] kb::Result<void> Start(const ThreadConfig& config) noexcept {
    static_assert(ThreadBody<Derived>,
                  "Derived must provide Setup, Step and Cleanup, each noexcept");
    kb::Status status = kb::Status::AlreadyStarted;
    if (state_.load(std::memory_order_acquire) != static_cast<uint8_t>(ThreadState::Idle)) {
      status = kb::Status::AlreadyStarted;
    } else if (config.name == nullptr) {
      status = kb::Status::InvalidArgument;
    } else if ((!wake_.IsCreated()) && (!wake_.Create(1U, 0U).IsOk())) {
      status = kb::Status::Fault;
    } else {
      period_ = config.period;
      stop_.store(false, std::memory_order_release);
      state_.store(static_cast<uint8_t>(ThreadState::Starting), std::memory_order_release);
      detail::ThreadParams params{};
      params.core = this;
      params.stack = stack_.data();
      params.stack_bytes = StackBytes;
      params.name = config.name;
      params.priority = config.priority;
      status = backend::ThreadCreate(storage_, params);
      if (status != kb::Status::Ok) {
        state_.store(static_cast<uint8_t>(ThreadState::Idle), std::memory_order_release);
      }
    }
    return kb::Result<void>::FromStatus(status);
  }

  /**
   * @brief Starts the thread and waits at most `timeout` (on the monotonic clock, plus at most a
   * tick) for Setup() to return.
   * @return Ok once the thread runs its loop (or already ended it after a good Setup); Fault when
   * Setup() failed; Timeout when it had not returned in time; else Start()'s error.
   */
  [[nodiscard]] kb::Result<void> StartAndWaitToVerify(const ThreadConfig& config,
                                                      Milliseconds timeout) noexcept {
    kb::Status status = Start(config).Error();
    if (status == kb::Status::Ok) {
      static_cast<void>(
          pollUntil([this]() noexcept { return State() != ThreadState::Starting; }, timeout));
      const ThreadState s = State();
      if (s == ThreadState::Failed) {
        status = kb::Status::Fault;
      } else if (s == ThreadState::Starting) {
        status = kb::Status::Timeout;
      } else {
        status = kb::Status::Ok;
      }
    }
    return kb::Result<void>::FromStatus(status);
  }

  /** @brief Asks the loop to end after the current step (a parked thread wakes to end). */
  void RequestStop() noexcept {
    stop_.store(true, std::memory_order_release);
    static_cast<void>(wake_.Give());
  }

  /** @brief Asks the loop to park after the current step, until Resume(). */
  void RequestPark() noexcept {
    park_.store(true, std::memory_order_release);
  }

  /** @brief Ends a park (or cancels a requested one). */
  void Resume() noexcept {
    park_.store(false, std::memory_order_release);
    static_cast<void>(wake_.Give());
  }

  /** @brief Resume() from an interrupt handler (ISR-safe). */
  void ResumeFromIsr() noexcept {
    park_.store(false, std::memory_order_release);
    static_cast<void>(wake_.GiveFromIsr());
  }

  /** @brief True while the loop is parked. */
  [[nodiscard]] bool IsParked() const noexcept {
    return parked_.load(std::memory_order_acquire);
  }

  /**
   * @brief The fewest free bytes the stack has had (high-water mark).
   * @return NotReady unless the thread is starting or running (the RTOS may have released an ended
   * thread); Unsupported where the backend cannot measure it (none, host, ThreadX without
   * TX_ENABLE_STACK_CHECKING).
   */
  [[nodiscard]] kb::Result<std::size_t> StackFreeBytes() const noexcept {
    std::size_t free_bytes{}; // written by the backend whenever it is read
    kb::Status status = kb::Status::NotReady;
    const ThreadState s = State();
    if ((s == ThreadState::Starting) || (s == ThreadState::Running)) {
      status = backend::ThreadStackFree(storage_, free_bytes);
    }
    return (status == kb::Status::Ok) ? kb::Result<std::size_t>::Success(free_bytes)
                                      : kb::Result<std::size_t>::Failure(status);
  }

  /** @brief True once a stop was requested. */
  [[nodiscard]] bool StopRequested() const noexcept {
    return stop_.load(std::memory_order_acquire);
  }

  /** @brief The current state. */
  [[nodiscard]] ThreadState State() const noexcept {
    return static_cast<ThreadState>(state_.load(std::memory_order_acquire));
  }

  /** @brief A snapshot of the statistics. */
  [[nodiscard]] ThreadStats Stats() const noexcept {
    ThreadStats s{};
    s.steps = steps_.load(std::memory_order_relaxed);
    s.overruns = overruns_.load(std::memory_order_relaxed);
    s.max_step_us = max_step_us_.load(std::memory_order_relaxed);
    return s;
  }

  /**
   * @brief Waits at most `timeout` (on the monotonic clock, plus at most a tick) for the thread to
   * end (Stopped or Failed), polling once a tick.
   * @return True when it ended.
   */
  [[nodiscard]] bool Join(Milliseconds timeout) noexcept {
    return pollUntil([this]() noexcept { return isEnded(); }, timeout);
  }

private:
  // Tests `done` once a tick until it holds or `timeout` has passed on the monotonic clock, so the
  // wait ends within a tick of the timeout at any tick rate. True when `done` held.
  template <typename Done>
  [[nodiscard]] static bool pollUntil(Done done, Milliseconds timeout) noexcept {
    constexpr uint64_t kUsPerMs = 1000U;
    return detail::PollUntil([]() noexcept { return backend::Now().Us(); },
                             []() noexcept { backend::SleepTicks(1U); }, done,
                             static_cast<uint64_t>(timeout.Count()) * kUsPerMs);
  }

  [[nodiscard]] bool isEnded() const noexcept {
    const ThreadState s = State();
    return (s == ThreadState::Stopped) || (s == ThreadState::Failed);
  }

  // The backend calls this with the ThreadCore that Start() passed: this object's own private base.
  static void runCore(detail::ThreadCore& core) noexcept {
    static_cast<Thread&>(core).run();
  }

  void run() noexcept {
    auto& body = static_cast<Derived&>(*this);
    ThreadState end = ThreadState::Failed;
    if (body.Setup()) {
      state_.store(static_cast<uint8_t>(ThreadState::Running), std::memory_order_release);
      loop(body);
      end = ThreadState::Stopped;
    }
    body.Cleanup();
    state_.store(static_cast<uint8_t>(end), std::memory_order_release);
    backend::ThreadExit(storage_);
  }

  void loop(Derived& body) noexcept {
    Periodic grid{};
    grid.Reset();
    bool more = true;
    while (more && (!StopRequested())) {
      const kb::Instant before = backend::Now();
      more = body.Step();
      record(kb::utils::ElapsedUs(backend::Now(), before));
      if (more && (!StopRequested())) {
        if (park_.load(std::memory_order_acquire)) {
          park();
          grid.Reset();
        } else {
          pace(grid);
        }
      }
    }
  }

  // Blocks on the wake semaphore until Resume() or RequestStop(); a stale give only re-tests.
  void park() noexcept {
    parked_.store(true, std::memory_order_release);
    while (park_.load(std::memory_order_acquire) && (!StopRequested())) {
      static_cast<void>(wake_.TakeFor(kb::kMaxMilliseconds));
    }
    parked_.store(false, std::memory_order_release);
  }

  void pace(Periodic& grid) noexcept {
    if (period_.Count() == 0U) {
      backend::Yield();
    } else if (!grid.Wait(period_)) {
      static_cast<void>(overruns_.fetch_add(1U, std::memory_order_relaxed));
    } else {
      // On time: nothing to record.
    }
  }

  void record(uint64_t step_us) noexcept {
    constexpr uint64_t kMax = std::numeric_limits<uint32_t>::max();
    const auto clipped = static_cast<uint32_t>(std::min(step_us, kMax));
    if (clipped > max_step_us_.load(std::memory_order_relaxed)) {
      max_step_us_.store(clipped, std::memory_order_relaxed);
    }
    static_cast<void>(steps_.fetch_add(1U, std::memory_order_relaxed));
  }

  detail::ThreadStorage storage_{};
  alignas(detail::kStackAlign) std::array<std::byte, StackBytes> stack_{};
  Milliseconds period_;
  std::atomic<uint8_t> state_{static_cast<uint8_t>(ThreadState::Idle)};
  Semaphore wake_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> park_{false};
  std::atomic<bool> parked_{false};
  std::atomic<uint32_t> steps_{0U};
  std::atomic<uint32_t> overruns_{0U};
  std::atomic<uint32_t> max_step_us_{0U};
};

} // namespace kb::rtos

#endif // KB_RTOS_THREAD_HPP
