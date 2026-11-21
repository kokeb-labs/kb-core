// kb::rtos on a PC over the std::thread backend: a CRTP thread on a 10 ms grid feeding a queue, a
// mutex with a bounded try-lock, event flags, a software timer and the at-least sleep. The same
// program runs on a target in the ESP32 port's kb_rtos_smoke example. Documentation snippets are
// taken from this file (docs/parts/rtos.md). SPDX-License-Identifier: MIT Copyright (c) 2026 The
// kb-core authors
#include <cstdint>
#include <cstdio>

#include "kb/rtos/rtos.hpp"
#include "kb/utils/utils.hpp"

namespace {

// --8<-- [start:objects]
// A thread is a class: Setup runs once, Step runs on the period until it returns false, Cleanup
// runs last. It gets the queue and the flags it works with by pointer, so the example needs no
// global.
class Ticker : public kb::rtos::Thread<Ticker, 4096U> {
public:
  Ticker(kb::rtos::Queue<uint32_t, 8>& queue, kb::rtos::EventFlags& flags) noexcept
      : queue_{&queue}, flags_{&flags} {}
  bool Setup() noexcept {
    count_ = 0U;
    return true;
  }
  bool Step() noexcept {
    const bool sent = queue_->SendFor(count_, kb::Milliseconds{10U});
    ++count_;
    return sent && (count_ < 5U);
  }
  void Cleanup() noexcept {
    flags_->Set(0x1U);
  }

private:
  kb::rtos::Queue<uint32_t, 8>* queue_;
  kb::rtos::EventFlags* flags_;
  uint32_t count_{0U};
};

void OnTimer(void* context) noexcept {
  static_cast<kb::rtos::EventFlags*>(context)->Set(0x2U);
}
// --8<-- [end:objects]

// --8<-- [start:checks]
bool RunChecks() {
  kb::rtos::Queue<uint32_t, 8> queue{};
  kb::rtos::Mutex mutex{};
  kb::rtos::EventFlags flags{};
  kb::rtos::Timer timer{};
  Ticker ticker{queue, flags};
  bool ok = queue.Create().IsOk() && mutex.Create().IsOk() && flags.Create().IsOk();
  ok = ok && timer.Create(&OnTimer, &flags, kb::Milliseconds{20U}, false).IsOk() && timer.Start();
  kb::rtos::ThreadConfig config{};
  config.name = "kb_ticker";
  config.priority = 5U;
  config.period = kb::Milliseconds{10U};
  ok = ok && ticker.Start(config).IsOk();
  uint32_t value = 0U;
  for (uint32_t expected = 0U; ok && (expected < 5U); ++expected) {
    ok = queue.ReceiveFor(value, kb::Milliseconds{100U}) && (value == expected);
  }
  ok = ok && ((flags.WaitAll(0x3U, kb::Milliseconds{200U}) & 0x3U) == 0x3U);
  ok = ok && ticker.Join(kb::Milliseconds{100U});
  const kb::Instant before = kb::rtos::Now();
  kb::rtos::SleepFor(kb::Milliseconds{5U});
  ok = ok && (kb::utils::ElapsedUs(kb::rtos::Now(), before) >= 5000U);
  ok = ok && mutex.TryLockFor(kb::Milliseconds{1U});
  mutex.Unlock();
  return ok;
}
// --8<-- [end:checks]

} // namespace

int main() {
  const bool ok = RunChecks();
  static_cast<void>(std::puts(ok ? "kb::rtos checks: pass" : "kb::rtos checks: FAIL"));
  return ok ? 0 : 1;
}
