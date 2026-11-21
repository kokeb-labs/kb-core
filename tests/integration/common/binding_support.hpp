// Shared pieces of the binding integration tests: the RTOS clock as a kb::iface::MonotonicClock, a
// spin delay, the HookTiming over both, and OwnerLoop, a std::thread that runs an owner domain's
// service pass every millisecond. Test code (outside the MISRA claim). SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#ifndef KB_TESTS_INTEGRATION_BINDING_SUPPORT_HPP
#define KB_TESTS_INTEGRATION_BINDING_SUPPORT_HPP

#include <atomic>
#include <cstdint>
#include <thread>

#include "kb/handlers/handlers.hpp"
#include "kb/rtos/time.hpp"

namespace kb_it {

/// kb::rtos::Now() as a MonotonicClock.
struct RtosClock {
  [[nodiscard]] kb::Instant Now() const noexcept {
    return kb::rtos::Now();
  }
};

/// A spin delay that sleeps instead (host tests: at least the time asked).
struct RtosSpin {
  void WaitUs(kb::Microseconds us) noexcept {
    kb::rtos::SleepFor(us);
  }
};

/// The clock, the spin delay and the HookTiming over them, for one test.
struct Timing {
  RtosClock clock{};
  RtosSpin spin{};
  kb::handlers::HookTiming timing{kb::iface::MonotonicClockRef{clock},
                                  kb::iface::BusyWaitRef{spin}};
};

/// Runs `domain.Service(now, access)` every millisecond on its own thread until destroyed.
template <typename Domain, typename Access>
class OwnerLoop {
public:
  OwnerLoop(Domain& domain, Access& access)
      : domain_(domain), access_(access), thread_([this] { run(); }) {}
  OwnerLoop(const OwnerLoop&) = delete;
  OwnerLoop& operator=(const OwnerLoop&) = delete;
  ~OwnerLoop() {
    stop_.store(true);
    thread_.join();
  }

private:
  void run() noexcept {
    while (!stop_.load()) {
      static_cast<void>(domain_.Service(kb::rtos::Now(), access_));
      kb::rtos::SleepFor(kb::Milliseconds{1U});
    }
  }
  Domain& domain_;
  Access& access_;
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

/// The status of a handler's health record (Unknown when the read was torn).
template <typename Access>
[[nodiscard]] kb::handlers::HealthState StateOf(const Access& access) noexcept {
  const kb::Result<kb::handlers::HealthRecord> h = access.Health();
  return h.IsOk() ? h.Value().state : kb::handlers::HealthState::Unknown;
}

} // namespace kb_it

#endif
