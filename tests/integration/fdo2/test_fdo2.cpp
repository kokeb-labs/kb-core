// fdo2 through kb::handlers::fdo2: the power stage and the polled boot and identification, the
// oxygen snapshot, the app API, error mapping to the health record, recovery, and both thread
// models, over a kb::iface::UartPort backed by the driver's own byte-level model of the module
// (components/drivers/driver-fdo2/tests/mock_fdo2.hpp), whose virtual time follows the RTOS clock.
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include <cstddef>
#include <cstdint>
#include <span>

#include "binding_support.hpp"
#include "kb/handlers/bindings/fdo2.hpp"
#include "kb_test.hpp"
#include "mock_fdo2.hpp"

namespace {

using kb::handlers::HealthState;
namespace fh = kb::handlers::fdo2;
using fdo2_test::FaultKind;

/// kb::iface::UartPort over the driver's module model; the model's clock follows kb::rtos::Now().
class ModelUart {
public:
  fdo2_test::Module module{};
  bool up_ok{true};
  int writes{0};

  [[nodiscard]] kb::Status EnsureReady() noexcept {
    return up_ok ? kb::Status::Ok : kb::Status::NotReady;
  }
  [[nodiscard]] kb::Status Write(std::span<const uint8_t> tx) noexcept {
    sync();
    ++writes;
    module.HostWrite(tx.data(), tx.size());
    return kb::Status::Ok;
  }
  [[nodiscard]] kb::Result<std::size_t> Read(std::span<uint8_t> rx,
                                             kb::Milliseconds timeout) noexcept {
    sync();
    const uint64_t start = kb::rtos::Now().Us();
    std::size_t n = module.HostRead(rx.data(), rx.size(), 0U);
    while ((n == 0U) &&
           ((kb::rtos::Now().Us() - start) < (static_cast<uint64_t>(timeout.Count()) * 1000U))) {
      kb::rtos::SleepFor(kb::Milliseconds{1U});
      sync();
      n = module.HostRead(rx.data(), rx.size(), 0U);
    }
    return kb::Result<std::size_t>::Success(n);
  }
  [[nodiscard]] kb::Status FlushInput() noexcept {
    sync();
    module.FlushRx();
    return kb::Status::Ok;
  }
  [[nodiscard]] kb::Status SetBaud(uint32_t baud) noexcept {
    module.host_baud = baud;
    return kb::Status::Ok;
  }

private:
  void sync() noexcept {
    const uint64_t real = kb::rtos::Now().Us() - base_us_;
    if (real > module.now_us) {
      module.Elapse(real - module.now_us);
    }
  }
  uint64_t base_us_{kb::rtos::Now().Us()};
};
static_assert(kb::iface::UartPort<ModelUart>);
static_assert(kb::iface::UartBaudControl<ModelUart>);

/// The module's supply switch as a kb::iface::GpioOutput.
struct PowerSwitch {
  fdo2_test::Module* module{nullptr};
  int sets{0};
  [[nodiscard]] kb::Status Set(kb::iface::Level level) noexcept {
    ++sets;
    module->SetPower(level == kb::iface::Level::High);
    return kb::Status::Ok;
  }
  [[nodiscard]] kb::Status Toggle() noexcept {
    return Set(module->powered ? kb::iface::Level::Low : kb::iface::Level::High);
  }
};
static_assert(kb::iface::GpioOutput<PowerSwitch>);

using Handler = fh::Handler<ModelUart>;
using GuardedBus = kb::handlers::BusDomain<ModelUart, kb::handlers::ThreadModel::Guarded>;
using OwnerBus = kb::handlers::BusDomain<ModelUart, kb::handlers::ThreadModel::Owner>;
using GuardedIo = kb::handlers::Guarded<Handler, GuardedBus>;
using OwnedIo = kb::handlers::OwnerAccess<Handler, OwnerBus>;

constexpr kb::Milliseconds kWait{3000U};

fh::Config ModuleConfig() {
  fh::Config c{};
  c.chip.power_up_ms = 1000U; // the shortest the driver accepts; the model is silent for 1 s
  c.chip.power_off_ms = 10U;
  c.chip.logo_ms = 100U;
  c.chip.retries = 0U;
  c.handler.health.fault_after = 2U;
  c.handler.recovery.backoff = kb::Milliseconds{1U};
  return c;
}

/// A guarded handler on its own UART with the supply switch, created.
struct GuardedRig {
  kb_it::Timing t{};
  ModelUart uart{};
  PowerSwitch power{&uart.module};
  GuardedBus domain{uart};
  GuardedIo io;
  explicit GuardedRig(fh::Config cfg) : io(domain, uart, t.timing, WithPower(cfg, power)) {
    uart.module.knobs.logo_us = 50'000U;
    uart.module.scenario.o2_hpa = 150.0;
    static_cast<void>(domain.Create());
    static_cast<void>(io.Create());
  }
  static fh::Config WithPower(fh::Config cfg, PowerSwitch& power) {
    cfg.power.pin = kb::iface::GpioOutputRef{power};
    return cfg;
  }
};

bool NearMhpa(int32_t value, int32_t expected) {
  const int32_t d = value - expected;
  return (d > -2000) && (d < 2000);
}

KB_TEST("fdo2: guarded start powers the module, polls the boot and identifies it") {
  GuardedRig r{ModuleConfig()};
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Unknown);
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  const kb::handlers::StartupReport rep = r.io.Startup().Value();
  KB_CHECK(rep.complete);
  KB_CHECK_EQ(rep.stages, 2U);
  KB_CHECK_EQ(rep.kinds, kb::handlers::stage::kBoot | kb::handlers::stage::kLink |
                             kb::handlers::stage::kIdentify);
  KB_CHECK(rep.stage_us[1] >= 1000000U); // the power-up wait was polled, not slept
  KB_CHECK(r.uart.module.powered);
  KB_CHECK(r.power.sets > 0);
  KB_CHECK(r.uart.module.CountCommands("#VERS") >= 1U);
  const int before = r.uart.writes;
  KB_CHECK(r.io.EnsureStarted(kWait).IsOk()); // idempotent, no traffic
  KB_CHECK_EQ(r.uart.writes, before);
}

KB_TEST("fdo2: calls before a start send nothing and return NotReady") {
  GuardedRig r{ModuleConfig()};
  KB_CHECK(fh::MeasureOxygen(r.io, kWait).Error() == kb::Status::NotReady);
  KB_CHECK(r.io.Sample(kWait).Error() == kb::Status::NotReady);
  KB_CHECK_EQ(r.uart.writes, 0);
  KB_CHECK_EQ(r.power.sets, 0);
}

KB_TEST("fdo2: a dead UART fails the power stage, another device fails identification; a retry "
        "starts again") {
  GuardedRig r{ModuleConfig()};
  r.uart.up_ok = false;
  const kb::Result<void> first = r.io.EnsureStarted(kWait);
  KB_CHECK(first.Error() == kb::Status::BusError);
  KB_CHECK_EQ(first.Detail(), kb::handlers::DetailOf(fdo2::Error::Bus));
  KB_CHECK_EQ(r.io.Startup().Value().stage, 0U);
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Faulted);
  r.uart.up_ok = true;
  r.uart.module.knobs.device_id = 9;
  const kb::Result<void> second = r.io.EnsureStarted(kWait);
  KB_CHECK(second.Error() == kb::Status::Fault);
  KB_CHECK_EQ(second.Detail(), kb::handlers::DetailOf(fdo2::Error::DeviceIdMismatch));
  KB_CHECK_EQ(r.io.Startup().Value().stage, 1U);
  r.uart.module.knobs.device_id = 8;
  KB_CHECK(r.io.EnsureStarted(kWait).IsOk());
  KB_CHECK_EQ(r.io.Health().Value().start_failures, 2U);
}

KB_TEST("fdo2: the snapshot carries the oxygen partial pressure and the temperature") {
  GuardedRig r{ModuleConfig()};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  const auto snap = r.io.Sample(kWait);
  KB_REQUIRE(snap.IsOk());
  KB_CHECK(NearMhpa(snap.Value().value.oxygen.o2_mhpa, 150000));
  KB_CHECK(snap.Value().value.oxygen.warming_up); // less than 3 minutes since power-up
  KB_CHECK(r.io.Latest().IsOk());
  r.uart.module.scenario.o2_hpa = 210.0;
  const auto now = fh::MeasureOxygen(r.io, kWait);
  KB_REQUIRE(now.IsOk());
  KB_CHECK(NearMhpa(now.Value().o2_mhpa, 210000));
}

KB_TEST("fdo2: raw reading, identify, resync, power cycle and a refused calibration through the "
        "app API") {
  GuardedRig r{ModuleConfig()};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  const auto raw = fh::MeasureRaw(r.io, kWait);
  KB_REQUIRE(raw.IsOk());
  KB_CHECK(raw.Value().pressure_ubar > 900000);
  KB_CHECK(fh::Identify(r.io, kWait).IsOk());
  KB_CHECK_EQ(r.uart.module.CountCommands("#LOGO"), 1U);
  KB_CHECK(fh::Resync(r.io, kWait).IsOk());
  const uint32_t vers = r.uart.module.CountCommands("#VERS");
  KB_CHECK(fh::PowerCycle(r.io, kWait).IsOk());
  KB_CHECK(r.uart.module.CountCommands("#VERS") > vers); // identified again after the cycle
  KB_CHECK(r.uart.module.powered);
  const auto cal = fh::CalibrateAir(r.io, 300U, kWait); // no consent for flash writes
  KB_CHECK(cal.Error() == kb::Status::InvalidArgument);
  KB_CHECK_EQ(cal.Detail(), kb::handlers::DetailOf(fdo2::Error::NotPermitted));
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready); // a caller's mistake, not a module failure
  KB_CHECK_EQ(r.io.Health().Value().refused_calls, 1U);
}

KB_TEST("fdo2: module refusals degrade, timeouts reach the threshold and fault, Recover() restores "
        "Ready") {
  GuardedRig r{ModuleConfig()};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  KB_REQUIRE(r.io.Sample(kWait).IsOk());
  r.uart.module.Arm(FaultKind::ErrorCode, -21); // "not executed": a clean refusal
  const auto bad = r.io.Sample(kWait);
  KB_CHECK(!bad.IsOk());
  KB_CHECK(bad.Error() == kb::Status::Fault);
  KB_CHECK_EQ(bad.Detail(), kb::handlers::DetailOf(fdo2::Error::DeviceError));
  KB_CHECK_EQ(r.io.Health().Value().last_detail, bad.Detail());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Degraded);
  KB_CHECK(r.io.Sample(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  r.uart.module.Arm(FaultKind::TruncatedAt, 5, 2);
  const auto cut = r.io.Sample(kWait);
  KB_CHECK(cut.Error() == kb::Status::Timeout);
  KB_CHECK_EQ(cut.Detail(), kb::handlers::DetailOf(fdo2::Error::Timeout));
  KB_CHECK(!r.io.Sample(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Faulted);
  const int before = r.uart.writes;
  KB_CHECK(r.io.Sample(kWait).Error() == kb::Status::NotReady);
  KB_CHECK_EQ(r.uart.writes, before); // a faulted handler sends nothing
  KB_CHECK(r.io.RequestRecovery(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  KB_CHECK_EQ(r.io.Health().Value().recoveries, 1U);
}

KB_TEST("fdo2: a potentially fatal module error faults at once") {
  GuardedRig r{ModuleConfig()};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  r.uart.module.Arm(FaultKind::ErrorCode, -77); // a code the datasheet does not list
  const auto res = fh::MeasureOxygen(r.io, kWait);
  KB_CHECK(res.Error() == kb::Status::Fault);
  KB_CHECK_EQ(res.Detail(), kb::handlers::DetailOf(fdo2::Error::DeviceFault));
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Faulted);
  KB_CHECK(r.io.RequestRecovery(kWait).IsOk());
  KB_CHECK(fh::MeasureOxygen(r.io, kWait).IsOk());
}

KB_TEST("fdo2: owner model: the owner task polls the start, samples and serves requests") {
  kb_it::Timing t{};
  ModelUart uart{};
  uart.module.scenario.o2_hpa = 100.0;
  OwnerBus domain{uart};
  fh::Config cfg = ModuleConfig();
  cfg.handler.sample_period = kb::Milliseconds{50U};
  OwnedIo io{domain, uart, t.timing, cfg};
  KB_REQUIRE(io.Create().IsOk());
  const kb_it::OwnerLoop<OwnerBus, OwnedIo> owner{domain, io};
  KB_REQUIRE(io.EnsureStarted(kWait).IsOk());
  bool seen = false;
  for (int i = 0; (i < 200) && (!seen); ++i) {
    kb::rtos::SleepFor(kb::Milliseconds{5U});
    const auto latest = io.Latest();
    seen = latest.IsOk() && NearMhpa(latest.Value().value.oxygen.o2_mhpa, 100000);
  }
  KB_CHECK(seen);
  KB_CHECK(fh::MeasureRaw(io, kWait).IsOk());
  KB_CHECK(io.Liveness().Beats() > 0U);
  KB_CHECK(io.Stop(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(io) == HealthState::Unknown);
}

} // namespace
