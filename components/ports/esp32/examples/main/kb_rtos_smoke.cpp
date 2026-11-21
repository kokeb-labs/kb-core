// kb::rtos on an ESP32 target over FreeRTOS: a CRTP thread on a 10 ms grid feeding a queue, a mutex
// with a bounded try-lock, event flags, a software timer and the at-least sleep, checked and logged
// once. SPDX-License-Identifier: MIT Copyright (c) 2026 The kb-core authors
#include <cstdint>

#include "esp_log.h"
#include "kb/rtos/rtos.hpp"
#include "kb/utils/fixed_string.hpp"

namespace {

constexpr const char* kTag = "kb_rtos_smoke";

kb::rtos::Queue<uint32_t, 8> g_queue{};
kb::rtos::Mutex g_mutex{};
kb::rtos::EventFlags g_flags{};
kb::rtos::Timer g_timer{};

class Ticker : public kb::rtos::Thread<Ticker, 4096U> {
public:
  bool Setup() noexcept {
    return true;
  }
  bool Step() noexcept {
    const bool sent = g_queue.SendFor(count_, kb::Milliseconds{10U});
    ++count_;
    return sent && (count_ < 5U);
  }
  void Cleanup() noexcept {
    g_flags.Set(0x1U);
  }

private:
  uint32_t count_{0U};
};

Ticker g_ticker{};

void OnTimer(void* context) noexcept {
  static_cast<kb::rtos::EventFlags*>(context)->Set(0x2U);
}

bool RunChecks() {
  bool ok = g_queue.Create().IsOk() && g_mutex.Create().IsOk() && g_flags.Create().IsOk();
  ok = ok && g_timer.Create(&OnTimer, &g_flags, kb::Milliseconds{20U}, false).IsOk() &&
       g_timer.Start();
  kb::rtos::ThreadConfig config{};
  config.name = "kb_ticker";
  config.priority = 5U;
  config.period = kb::Milliseconds{10U};
  ok = ok && g_ticker.Start(config).IsOk();
  uint32_t value = 0U;
  for (uint32_t expected = 0U; ok && (expected < 5U); ++expected) {
    ok = g_queue.ReceiveFor(value, kb::Milliseconds{100U}) && (value == expected);
  }
  ok = ok && ((g_flags.WaitAll(0x3U, kb::Milliseconds{200U}) & 0x3U) == 0x3U);
  ok = ok && g_ticker.Join(kb::Milliseconds{100U});
  const kb::Instant before = kb::rtos::Now();
  kb::rtos::SleepFor(kb::Milliseconds{5U});
  ok = ok && (kb::utils::ElapsedUs(kb::rtos::Now(), before) >= 5000U);
  ok = ok && g_mutex.TryLockFor(kb::Milliseconds{1U});
  g_mutex.Unlock();
  return ok;
}

} // namespace

extern "C" void app_main(void) {
  kb::utils::FixedString<64> line{};
  const bool ok = RunChecks();
  static_cast<void>(line.Append("kb::rtos checks: "));
  static_cast<void>(line.Append(ok ? "pass" : "FAIL"));
  ESP_LOGI(kTag, "%s", line.CStr());
}
