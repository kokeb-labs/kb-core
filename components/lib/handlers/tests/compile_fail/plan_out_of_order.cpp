// A plan with stages out of kind order (Configure before Identify) does not compile (ADR-0010).
// EXPECT: (kStartupPlan|static assertion failed|static_assert)
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include <array>
#include <cstddef>
#include <optional>

#include "kb/handlers/handlers.hpp"

struct Drv {
  int unused{0};
};

class Backwards : public kb::handlers::HandlerCore<Backwards, Drv> {
public:
  static constexpr std::array<kb::handlers::Stage, 2> kStartupPlan{
      {{kb::handlers::stage::kConfigure, kb::handlers::StageMode::Sync, false},
       {kb::handlers::stage::kIdentify, kb::handlers::StageMode::Sync, false}}};
  static constexpr bool kRequiresOwner = false;
  static constexpr kb::handlers::BusKind kBus = kb::handlers::BusKind::I2c;
  Backwards() noexcept : HandlerCore(kb::handlers::HandlerConfig{}) {}
  void EmplaceDriver(std::optional<Drv>& slot) noexcept {
    slot.emplace();
  }
  [[nodiscard]] kb::Result<kb::handlers::StartProgress> RunStage(
      const kb::handlers::StageCall& call, Drv& driver) noexcept {
    static_cast<void>(call);
    static_cast<void>(driver);
    return kb::Result<kb::handlers::StartProgress>::Success(kb::handlers::StartProgress::Done);
  }
  [[nodiscard]] kb::Milliseconds StageDeadline(std::size_t index) const noexcept {
    static_cast<void>(index);
    return kb::Milliseconds{1U};
  }
};

[[nodiscard]] kb::handlers::StartProgress Use(Backwards& b, const kb::handlers::ServiceKey& key) {
  return b.StartStep(key, kb::Instant{}).Value();
}
