// ntc_thermistor through kb::handlers::ntc_thermistor: start-up, the app API, error mapping to the
// health record, recovery, and both thread models, over a kb::iface::AdcChannel backed by the
// driver's own ADC model (components/drivers/driver-ntc-thermistor/tests/mock_ntc_thermistor.hpp)
// and a host pin for the bias. SPDX-License-Identifier: MIT Copyright (c) 2026 The kb-core authors
#include <cstdint>

#include "binding_support.hpp"
#include "kb/handlers/bindings/ntc_thermistor.hpp"
#include "kb/interfaces/host/fake_pins.hpp"
#include "kb_test.hpp"
#include "mock_ntc_thermistor.hpp"
#include "ntc_thermistor_presets.hpp"

namespace {

using kb::handlers::HealthState;
namespace binding = kb::handlers::ntc_thermistor;

/// kb::iface::AdcChannel over the driver's ADC model: the raw codes it returns in turn.
class ModelAdc {
public:
  ntc_thermistor_test::AdcState state{};
  [[nodiscard]] kb::Result<uint32_t> ReadRaw() noexcept {
    uint32_t code = 0U;
    const bool ok = state.Read(0U, code);
    return ok ? kb::Result<uint32_t>::Success(code)
              : kb::Result<uint32_t>::Failure(kb::Status::BusError);
  }
  [[nodiscard]] kb::Result<int32_t> ReadMillivolts() noexcept {
    return kb::Result<int32_t>::Failure(kb::Status::Unsupported);
  }
  [[nodiscard]] uint32_t FullScale() const noexcept {
    return 4095U;
  }
  [[nodiscard]] int Reads() const noexcept {
    return state.reads;
  }
};
static_assert(kb::iface::AdcChannel<ModelAdc>);

using Handler = binding::Handler<ModelAdc>;
using GuardedBus = kb::handlers::BusDomain<ModelAdc, kb::handlers::ThreadModel::Guarded>;
using OwnerBus = kb::handlers::BusDomain<ModelAdc, kb::handlers::ThreadModel::Owner>;
using GuardedIo = kb::handlers::Guarded<Handler, GuardedBus>;
using OwnedIo = kb::handlers::OwnerAccess<Handler, OwnerBus>;

constexpr kb::Milliseconds kWait{200U};
constexpr uint32_t kMidCode = 2048U; // 10 kOhm against the 10 kOhm series resistor: 25 degC

static_assert(binding::ToStatus(ntc_thermistor::Error::OpenCircuit) == kb::Status::NoDevice);
static_assert(binding::ToStatus(ntc_thermistor::Error::AboveRange) == kb::Status::OutOfRange);
static_assert(binding::ToStatus(ntc_thermistor::Error::NotInitialized) == kb::Status::NotReady);

/// The generic 10 kOhm / B3950 bead, low side, 12-bit ratiometric, averaging four codes.
binding::Config Bead() {
  binding::Config c{};
  c.chip.curve = ntc_thermistor::presets::kGeneric10kB3950;
  c.chip.min_temperature_mc = -40'000;
  c.chip.max_temperature_mc = 125'000;
  c.chip.sample_count = 4U;
  c.chip.sample_delay_us = 20U;
  c.handler.health.fault_after = 2U;
  c.handler.recovery.backoff = kb::Milliseconds{1U};
  return c;
}

/// A guarded handler on its own channel, created.
struct GuardedRig {
  kb_it::Timing t{};
  ModelAdc adc{};
  GuardedBus domain{adc};
  GuardedIo io;
  explicit GuardedRig(const binding::Config& cfg) : io(domain, adc, t.timing, cfg) {
    adc.state.Fixed(kMidCode);
    static_cast<void>(domain.Create());
    static_cast<void>(io.Create());
  }
};

bool Near(int32_t a, int32_t b, int32_t tolerance) {
  const int32_t d = a - b;
  return (d <= tolerance) && (d >= -tolerance);
}

KB_TEST("ntc_thermistor: guarded start runs the one sync stage, converts nothing, then samples") {
  GuardedRig r{Bead()};
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Unknown);
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  const kb::handlers::StartupReport report = r.io.Startup().Value();
  KB_CHECK(report.complete);
  KB_CHECK_EQ(report.stages, 1U);
  KB_CHECK_EQ(report.kinds, kb::handlers::stage::kIdentify | kb::handlers::stage::kConfigure);
  KB_CHECK_EQ(r.adc.Reads(), 0); // the start checks the configuration only
  KB_CHECK(r.io.EnsureStarted(kWait).IsOk());
  const auto snap = r.io.Sample(kWait);
  KB_REQUIRE(snap.IsOk());
  KB_CHECK(Near(snap.Value().value.temperature_mc, 25'000, 100));
  KB_CHECK(Near(static_cast<int32_t>(snap.Value().value.resistance_ohm), 10'000, 20));
  KB_CHECK_EQ(snap.Value().value.code, kMidCode);
  KB_CHECK_EQ(r.adc.Reads(), 4);
  KB_CHECK(r.io.Latest().IsOk());
}

KB_TEST("ntc_thermistor: calls before a start convert nothing and return NotReady") {
  GuardedRig r{Bead()};
  KB_CHECK(binding::ReadTemperature(r.io, kWait).Error() == kb::Status::NotReady);
  KB_CHECK(binding::ReadRawCode(r.io, kWait).Error() == kb::Status::NotReady);
  KB_CHECK(binding::ReadResistance(r.io, kWait).Error() == kb::Status::NotReady);
  KB_CHECK_EQ(r.adc.Reads(), 0);
}

KB_TEST("ntc_thermistor: a configuration without a window fails the start with InvalidArgument") {
  binding::Config cfg = Bead();
  cfg.chip.min_temperature_mc = ntc_thermistor::kNoMinTemperatureMc; // beta mode needs a window
  GuardedRig r{cfg};
  const kb::Result<void> first = r.io.EnsureStarted(kWait);
  KB_CHECK(first.Error() == kb::Status::InvalidArgument);
  KB_CHECK_EQ(first.Detail(), kb::handlers::DetailOf(ntc_thermistor::Error::InvalidArgument));
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Faulted);
  KB_CHECK(!r.io.Startup().Value().complete);
}

KB_TEST("ntc_thermistor: a failed bias pin fails the start with BusError; a retry starts again") {
  kb_it::Timing t{};
  ModelAdc adc{};
  adc.state.Fixed(kMidCode);
  kb::iface::host::FakeGpioPin bias{};
  bias.Drive(kb::iface::Level::High);
  GuardedBus domain{adc};
  binding::Config cfg = Bead();
  cfg.bias.pin = kb::iface::GpioOutputRef{bias};
  cfg.chip.settle_us = 100U;
  GuardedIo io{domain, adc, t.timing, cfg};
  KB_REQUIRE(domain.Create().IsOk());
  KB_REQUIRE(io.Create().IsOk());
  bias.FailNext(kb::Status::BusError);
  const kb::Result<void> bad = io.EnsureStarted(kWait);
  KB_CHECK(bad.Error() == kb::Status::BusError);
  KB_CHECK_EQ(bad.Detail(), kb::handlers::DetailOf(ntc_thermistor::Error::Bus));
  KB_CHECK_EQ(io.Startup().Value().stage, 0U);
  KB_REQUIRE(io.EnsureStarted(kWait).IsOk());
  KB_CHECK(bias.LevelNow() == kb::iface::Level::Low); // the divider is off between readings
  const uint32_t writes = bias.Writes();
  KB_CHECK(binding::ReadTemperature(io, kWait).IsOk());
  KB_CHECK_EQ(bias.Writes(), writes + 2U); // on for the reading, off after it
  KB_CHECK(bias.LevelNow() == kb::iface::Level::Low);
}

KB_TEST("ntc_thermistor: the commands reach the model") {
  GuardedRig r{Bead()};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  r.adc.state.Fixed(1000U);
  const kb::Result<uint32_t> code = binding::ReadRawCode(r.io, kWait);
  KB_REQUIRE(code.IsOk());
  KB_CHECK_EQ(code.Value(), 1000U);
  const kb::Result<uint32_t> ohm = binding::ReadResistance(r.io, kWait);
  KB_REQUIRE(ohm.IsOk());
  KB_CHECK(Near(static_cast<int32_t>(ohm.Value()), 3'230, 20)); // 10k x 1000 / 3096: hotter
  const kb::Result<int32_t> mc = binding::ReadTemperature(r.io, kWait);
  KB_REQUIRE(mc.IsOk());
  KB_CHECK(mc.Value() > 25'000);
}

KB_TEST("ntc_thermistor: an ADC failure degrades, an open circuit faults at the threshold, "
        "Recover() restores Ready") {
  GuardedRig r{Bead()};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  r.adc.state.fail_read_at = r.adc.state.reads;
  const kb::Result<int32_t> bad = binding::ReadTemperature(r.io, kWait);
  KB_CHECK(bad.Error() == kb::Status::BusError);
  KB_CHECK_EQ(bad.Detail(), kb::handlers::DetailOf(ntc_thermistor::Error::Bus));
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Degraded);
  KB_CHECK(binding::ReadTemperature(r.io, kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  r.adc.state.Fixed(4095U); // low side: the node at the supply, the thermistor is open
  const kb::Result<int32_t> open = binding::ReadTemperature(r.io, kWait);
  KB_CHECK(open.Error() == kb::Status::NoDevice);
  KB_CHECK_EQ(open.Detail(), kb::handlers::DetailOf(ntc_thermistor::Error::OpenCircuit));
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Degraded);
  KB_CHECK(!binding::ReadTemperature(r.io, kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Faulted);
  const int before = r.adc.Reads();
  KB_CHECK(binding::ReadRawCode(r.io, kWait).Error() == kb::Status::NotReady);
  KB_CHECK_EQ(r.adc.Reads(), before);
  r.adc.state.Fixed(kMidCode);
  KB_CHECK(r.io.RequestRecovery(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  KB_CHECK_EQ(r.io.Health().Value().recoveries, 1U);
}

KB_TEST("ntc_thermistor: NotInitialized is the fatal error; circuit errors are not") {
  kb_it::Timing t{};
  ModelAdc adc{};
  const Handler h{adc, t.timing, Bead()};
  KB_CHECK(h.IsFatal(kb::Status::NotReady,
                     kb::handlers::DetailOf(ntc_thermistor::Error::NotInitialized)));
  KB_CHECK(
      !h.IsFatal(kb::Status::Fault, kb::handlers::DetailOf(ntc_thermistor::Error::ShortCircuit)));
}

KB_TEST("ntc_thermistor: owner model: the owner task starts, samples and serves requests") {
  kb_it::Timing t{};
  ModelAdc adc{};
  adc.state.Fixed(kMidCode);
  OwnerBus domain{adc};
  binding::Config cfg = Bead();
  cfg.handler.sample_period = kb::Milliseconds{2U};
  OwnedIo io{domain, adc, t.timing, cfg};
  KB_REQUIRE(io.Create().IsOk());
  const kb_it::OwnerLoop<OwnerBus, OwnedIo> owner{domain, io};
  KB_REQUIRE(io.EnsureStarted(kWait).IsOk());
  const kb::Result<int32_t> mc = binding::ReadTemperature(io, kWait);
  KB_REQUIRE(mc.IsOk());
  KB_CHECK(Near(mc.Value(), 25'000, 100));
  KB_CHECK(binding::ReadRawCode(io, kWait).IsOk());
  KB_CHECK(io.Latest().IsOk());
  KB_CHECK(io.Liveness().Beats() > 0U);
  KB_CHECK(io.Stop(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(io) == HealthState::Unknown);
}

} // namespace
