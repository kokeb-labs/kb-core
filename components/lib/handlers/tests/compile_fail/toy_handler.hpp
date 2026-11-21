// A minimal handler for the compile-fail tests: one sync stage, I2C, `RequiresOwner` chosen per
// use. SPDX-License-Identifier: MIT Copyright (c) 2026 The kb-core authors
#ifndef KB_HANDLERS_TESTS_COMPILE_FAIL_TOY_HANDLER_HPP
#define KB_HANDLERS_TESTS_COMPILE_FAIL_TOY_HANDLER_HPP

#include <array>
#include <cstddef>
#include <optional>

#include "kb/handlers/handlers.hpp"
#include "kb/interfaces/host/host.hpp"

struct ToyDriver {
  int unused{0};
};

template <bool RequiresOwner, kb::handlers::BusKind Bus = kb::handlers::BusKind::I2c>
class Toy : public kb::handlers::HandlerCore<Toy<RequiresOwner, Bus>, ToyDriver> {
public:
  static constexpr std::array<kb::handlers::Stage, 1> kStartupPlan{
      {{kb::handlers::stage::kIdentify, kb::handlers::StageMode::Sync, false}}};
  static constexpr bool kRequiresOwner = RequiresOwner;
  static constexpr kb::handlers::BusKind kBus = Bus;
  Toy() noexcept : kb::handlers::HandlerCore<Toy, ToyDriver>(kb::handlers::HandlerConfig{}) {}
  void EmplaceDriver(std::optional<ToyDriver>& slot) noexcept {
    slot.emplace();
  }
  [[nodiscard]] kb::Result<kb::handlers::StartProgress> RunStage(
      const kb::handlers::StageCall& call, ToyDriver& driver) noexcept {
    static_cast<void>(call);
    static_cast<void>(driver);
    return kb::Result<kb::handlers::StartProgress>::Success(kb::handlers::StartProgress::Done);
  }
  [[nodiscard]] kb::Milliseconds StageDeadline(std::size_t index) const noexcept {
    static_cast<void>(index);
    return kb::Milliseconds{1U};
  }
};

using Bus = kb::iface::host::FakeI2cBus;
using OwnerDomain = kb::handlers::BusDomain<Bus, kb::handlers::ThreadModel::Owner>;
using GuardedDomain = kb::handlers::BusDomain<Bus, kb::handlers::ThreadModel::Guarded>;

#endif
