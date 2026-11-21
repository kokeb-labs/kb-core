// ads7952 through kb::handlers::ads7952: start-up, the app API, error mapping to the health record,
// recovery, and both thread models (the owner one sampling Auto-1 scans), over a
// kb::iface::SpiDevice backed by the driver's own chip model
// (components/drivers/driver-ads7952/tests/mock_ads7952_bus.hpp). SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include <cstdint>
#include <span>

#include "binding_support.hpp"
#include "kb/handlers/bindings/ads7952.hpp"
#include "kb_test.hpp"
#include "mock_ads7952_bus.hpp"

namespace {

using kb::handlers::HealthState;
namespace binding = kb::handlers::ads7952;

/// kb::iface::SpiDevice over the chip model; the model's time follows the RTOS clock.
class ModelSpi {
public:
  ads7952_test::MockBusState state{};

  [[nodiscard]] kb::Status EnsureReady() noexcept {
    return state.bus_up_ok ? kb::Status::Ok : kb::Status::NotReady;
  }
  [[nodiscard]] kb::Status Transfer(std::span<const uint8_t> tx, std::span<uint8_t> rx) noexcept {
    const uint64_t now = kb::rtos::Now().Us();
    state.chip.Elapse(static_cast<uint32_t>(now - last_us_));
    last_us_ = now;
    const bool ok = state.DoTransfer(tx.data(), rx.data(), tx.size());
    return ok ? kb::Status::Ok : kb::Status::BusError;
  }

private:
  uint64_t last_us_{kb::rtos::Now().Us()};
};
static_assert(kb::iface::SpiDevice<ModelSpi>);

using Handler = binding::Handler<ModelSpi>;
using GuardedBus = kb::handlers::BusDomain<ModelSpi, kb::handlers::ThreadModel::Guarded>;
using OwnerBus = kb::handlers::BusDomain<ModelSpi, kb::handlers::ThreadModel::Owner>;
using GuardedIo = kb::handlers::Guarded<Handler, GuardedBus>;
using OwnedIo = kb::handlers::OwnerAccess<Handler, OwnerBus>;

constexpr kb::Milliseconds kWait{200U};

static_assert(binding::ToStatus(ads7952::Error::DeviceReset) == kb::Status::NotReady);
static_assert(binding::ToStatus(ads7952::Error::ChannelMismatch) == kb::Status::BusError);
static_assert(binding::ToStatus(ads7952::Error::NoDevice) == kb::Status::NoDevice);

binding::Config Defaults() {
  binding::Config c{};
  c.chip.auto1_channels = 0x0F0EU; // channel 0 (the power-on channel) not in the sequence
  c.chip.gpio.output_mask = 0x01U; // GPIO0 an output
  c.handler.health.fault_after = 2U;
  c.handler.recovery.backoff = kb::Milliseconds{1U};
  return c;
}

/// A guarded handler on its own chip select, created.
struct GuardedRig {
  kb_it::Timing t{};
  ModelSpi spi{};
  GuardedBus domain{spi};
  GuardedIo io;
  explicit GuardedRig(const binding::Config& cfg) : io(domain, spi, t.timing, cfg) {
    static_cast<void>(domain.Create());
    static_cast<void>(io.Create());
  }
};

KB_TEST("ads7952: guarded start runs the one sync stage and the handler is Ready") {
  GuardedRig r{Defaults()};
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Unknown);
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  const kb::handlers::StartupReport report = r.io.Startup().Value();
  KB_CHECK(report.complete);
  KB_CHECK_EQ(report.stages, 1U);
  KB_CHECK_EQ(report.kinds, kb::handlers::stage::kIdentify | kb::handlers::stage::kConfigure);
  KB_CHECK_EQ(r.spi.state.chip.auto1_mask, 0x0F0EU); // the program reached the chip
  const int transfers = r.spi.state.transfers;
  KB_CHECK(r.io.EnsureStarted(kWait).IsOk()); // idempotent, no I/O
  KB_CHECK_EQ(r.spi.state.transfers, transfers);
}

KB_TEST("ads7952: calls before a start send nothing and return NotReady") {
  GuardedRig r{Defaults()};
  KB_CHECK(binding::ReadChannel(r.io, 3U, kWait).Error() == kb::Status::NotReady);
  KB_CHECK(r.io.Sample(kWait).Error() == kb::Status::NotReady);
  KB_CHECK_EQ(r.spi.state.transfers, 0);
}

KB_TEST("ads7952: an absent chip fails the start with NoDevice and the driver's code; a retry "
        "starts again") {
  GuardedRig r{Defaults()};
  r.spi.state.chip.miso = ads7952_test::Miso::FloatHigh;
  const kb::Result<void> first = r.io.EnsureStarted(kWait);
  KB_CHECK(first.Error() == kb::Status::NoDevice);
  KB_CHECK_EQ(first.Detail(), kb::handlers::DetailOf(ads7952::Error::NoDevice));
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Faulted);
  KB_CHECK_EQ(r.io.Health().Value().start_failures, 1U);
  KB_CHECK(!r.io.Startup().Value().complete);
  r.spi.state.chip.miso = ads7952_test::Miso::Chip;
  KB_CHECK(r.io.EnsureStarted(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
}

KB_TEST("ads7952: channel reads, scans, range, GPIO and the scale check reach the chip model") {
  binding::Config cfg = Defaults();
  cfg.chip.monitor.channel = 4U; // 700 mV on channel 4 in the model
  cfg.chip.monitor.expected_uv = 700'000U;
  cfg.chip.monitor.tolerance_uv = 20'000U;
  GuardedRig r{cfg};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  ads7952_test::Ads7952Model& m = r.spi.state.chip;
  const kb::Result<uint16_t> ch3 = binding::ReadChannel(r.io, 3U, kWait);
  KB_REQUIRE(ch3.IsOk());
  KB_CHECK_EQ(ch3.Value(), m.CodeOf(3U, false));
  const kb::Result<binding::Snapshot> scan = binding::ScanAuto1(r.io, kWait);
  KB_REQUIRE(scan.IsOk());
  KB_CHECK_EQ(scan.Value().mask, 0x0F0EU);
  KB_CHECK_EQ(scan.Value().code[9], m.CodeOf(9U, false));
  binding::Command c{};
  c.op = binding::Op::ScanAuto2;
  const auto auto2 = r.io.Request(c, kWait);
  KB_REQUIRE(auto2.IsOk());
  KB_CHECK_EQ(auto2.Value().scan.mask, 0x0FFFU); // channels 0 to 11
  const kb::Result<uint16_t> scale = binding::CheckScale(r.io, kWait);
  KB_REQUIRE(scale.IsOk());
  KB_CHECK_EQ(scale.Value(), m.CodeOf(4U, false));
  KB_REQUIRE(binding::SetGpioOutputs(r.io, 0x01U, kWait, ads7952::Verify::ReadBack).IsOk());
  KB_CHECK_EQ(m.gpo & 0x01U, 0x01U);
  const kb::Result<uint8_t> gpio = binding::ReadGpio(r.io, kWait);
  KB_REQUIRE(gpio.IsOk());
  KB_CHECK_EQ(gpio.Value() & 0x01U, 0x01U);
  KB_REQUIRE(binding::SetRange(r.io, ads7952::Range::TwoVref, kWait).IsOk());
  const auto snap = r.io.Sample(kWait);
  KB_REQUIRE(snap.IsOk());
  KB_CHECK(snap.Value().value.range == ads7952::Range::TwoVref);
  KB_CHECK_EQ(snap.Value().value.code[2], m.CodeOf(2U, true));
}

KB_TEST("ads7952: bus failures degrade, the threshold faults, Recover() restores Ready") {
  GuardedRig r{Defaults()};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  r.spi.state.fail_transfer = r.spi.state.transfers;
  const kb::Result<uint16_t> bad = binding::ReadChannel(r.io, 5U, kWait);
  KB_CHECK(bad.Error() == kb::Status::BusError);
  KB_CHECK_EQ(bad.Detail(), kb::handlers::DetailOf(ads7952::Error::Bus));
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Degraded);
  KB_CHECK(binding::ReadChannel(r.io, 5U, kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  r.spi.state.fail_transfer = r.spi.state.transfers;
  KB_CHECK(!binding::ReadChannel(r.io, 6U, kWait).IsOk());
  r.spi.state.fail_transfer = r.spi.state.transfers;
  KB_CHECK(!binding::ReadChannel(r.io, 6U, kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Faulted);
  const int before = r.spi.state.transfers;
  KB_CHECK(binding::ReadChannel(r.io, 6U, kWait).Error() == kb::Status::NotReady);
  KB_CHECK_EQ(r.spi.state.transfers, before);
  KB_CHECK(r.io.RequestRecovery(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  KB_CHECK_EQ(r.io.Health().Value().recoveries, 1U);
}

KB_TEST("ads7952: a brown-out seen in a scan is fatal at once; recovery programs the chip again") {
  GuardedRig r{Defaults()};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  KB_REQUIRE(r.io.Sample(kWait).IsOk());
  ads7952_test::Ads7952Model& m = r.spi.state.chip;
  m.brownout_before_frame = m.frames + 2;
  const auto res = r.io.Sample(kWait);
  KB_CHECK(res.Error() == kb::Status::NotReady);
  KB_CHECK_EQ(res.Detail(), kb::handlers::DetailOf(ads7952::Error::DeviceReset));
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Faulted);
  KB_CHECK(r.io.RequestRecovery(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  KB_CHECK_EQ(m.auto1_mask, 0x0F0EU);
  KB_CHECK(r.io.Sample(kWait).IsOk());
}

KB_TEST("ads7952: owner model: the owner task samples Auto-1 scans; Latest() is lock-free") {
  kb_it::Timing t{};
  ModelSpi spi{};
  OwnerBus domain{spi};
  binding::Config cfg = Defaults();
  cfg.handler.sample_period = kb::Milliseconds{1U};
  OwnedIo io{domain, spi, t.timing, cfg};
  KB_REQUIRE(io.Create().IsOk());
  const kb_it::OwnerLoop<OwnerBus, OwnedIo> owner{domain, io};
  KB_REQUIRE(io.EnsureStarted(kWait).IsOk());
  const kb::Result<uint16_t> ch = binding::ReadChannel(io, 7U, kWait);
  KB_REQUIRE(ch.IsOk());
  KB_CHECK_EQ(ch.Value(), spi.state.chip.CodeOf(7U, false));
  auto latest = io.Latest();
  for (uint32_t i = 0U; (i < 1000U) && (!latest.IsOk());
       ++i) { // the owner's first scan, under load
    kb::rtos::SleepFor(kb::Milliseconds{1U});
    latest = io.Latest();
  }
  KB_REQUIRE(latest.IsOk());
  KB_CHECK(latest.Value().sequence > 0U);
  KB_CHECK_EQ(latest.Value().value.mask, 0x0F0EU);
  KB_CHECK_EQ(latest.Value().value.code[1], spi.state.chip.CodeOf(1U, false));
  KB_CHECK(io.Stop(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(io) == HealthState::Unknown);
}

} // namespace
