// mcp9700 through kb::handlers::mcp9700: start-up, the app API, error mapping to the health record,
// recovery, and both thread models, over a kb::iface::AdcChannel backed by the driver's own sensor
// and ADC model (components/drivers/driver-mcp9700/tests/mock_mcp9700.hpp).
// SPDX-License-Identifier: MIT Copyright (c) 2026 The kb-core authors
#include <cstdint>

#include "binding_support.hpp"
#include "kb/handlers/bindings/mcp9700.hpp"
#include "kb/interfaces/host/fake_pins.hpp"
#include "kb_test.hpp"
#include "mock_mcp9700.hpp"

namespace {

using kb::handlers::HealthState;
namespace binding = kb::handlers::mcp9700;

/// The driver's sensor model; its time follows the RTOS clock, so real waits settle the sensor.
struct ModelSensor {
  mcp9700_test::MockBusState state{};
  uint64_t origin_us{kb::rtos::Now().Us()};

  void Sync() noexcept {
    const auto now = static_cast<int64_t>(kb::rtos::Now().Us() - origin_us);
    state.time_us = (now > state.time_us) ? now : state.time_us;
  }
};

/// kb::iface::AdcChannel over the model: calibrated millivolts of the wired channel.
class ModelAdc {
public:
  explicit ModelAdc(ModelSensor& sensor) noexcept : sensor_(sensor) {}
  [[nodiscard]] kb::Result<uint32_t> ReadRaw() noexcept {
    return kb::Result<uint32_t>::Failure(kb::Status::Unsupported);
  }
  [[nodiscard]] kb::Result<int32_t> ReadMillivolts() noexcept {
    sensor_.Sync();
    uint32_t mv = 0U;
    const bool ok = sensor_.state.Convert(sensor_.state.wired_channel, mv);
    return ok ? kb::Result<int32_t>::Success(static_cast<int32_t>(mv))
              : kb::Result<int32_t>::Failure(kb::Status::BusError);
  }
  [[nodiscard]] uint32_t FullScale() const noexcept {
    return 4095U;
  }

private:
  ModelSensor& sensor_;
};
static_assert(kb::iface::AdcChannel<ModelAdc>);

/// The sensor's supply pin: a host fake (fault injection) that powers the model.
class SupplyPin {
public:
  explicit SupplyPin(ModelSensor& sensor) noexcept : sensor_(sensor) {
    sensor_.state.powered = false;
  }
  kb::iface::host::FakeGpioPin pin{};
  [[nodiscard]] kb::Status Set(kb::iface::Level level) noexcept {
    const kb::Status s = pin.Set(level);
    if (s == kb::Status::Ok) {
      sensor_.Sync();
      sensor_.state.SetPower(level == kb::iface::Level::High);
    }
    return s;
  }
  [[nodiscard]] kb::Status Toggle() noexcept {
    return Set((pin.LevelNow() == kb::iface::Level::High) ? kb::iface::Level::Low
                                                          : kb::iface::Level::High);
  }

private:
  ModelSensor& sensor_;
};
static_assert(kb::iface::GpioOutput<SupplyPin>);

using Handler = binding::Handler<ModelAdc>;
using GuardedBus = kb::handlers::BusDomain<ModelAdc, kb::handlers::ThreadModel::Guarded>;
using OwnerBus = kb::handlers::BusDomain<ModelAdc, kb::handlers::ThreadModel::Owner>;
using GuardedIo = kb::handlers::Guarded<Handler, GuardedBus>;
using OwnedIo = kb::handlers::OwnerAccess<Handler, OwnerBus>;

constexpr kb::Milliseconds kWait{500U};

static_assert(binding::ToStatus(mcp9700::Error::OutputLow) == kb::Status::OutOfRange);
static_assert(binding::ToStatus(mcp9700::Error::Bus) == kb::Status::BusError);
static_assert(binding::ToStatus(mcp9700::Error::NotInitialized) == kb::Status::NotReady);

binding::Config Defaults() {
  binding::Config c{};
  c.chip.sample_interval_us = 50U;
  c.handler.health.fault_after = 2U;
  c.handler.recovery.backoff = kb::Milliseconds{1U};
  return c;
}

/// A guarded handler on its own channel, created.
struct GuardedRig {
  kb_it::Timing t{};
  ModelSensor sensor{};
  ModelAdc adc{sensor};
  GuardedBus domain{adc};
  GuardedIo io;
  explicit GuardedRig(const binding::Config& cfg) : io(domain, adc, t.timing, cfg) {
    static_cast<void>(domain.Create());
    static_cast<void>(io.Create());
  }
};

bool Near(int32_t a, int32_t b, int32_t tolerance) {
  const int32_t d = a - b;
  return (d <= tolerance) && (d >= -tolerance);
}

KB_TEST("mcp9700: guarded start runs the one sync stage, the handler is Ready and samples") {
  GuardedRig r{Defaults()};
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Unknown);
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  const kb::handlers::StartupReport report = r.io.Startup().Value();
  KB_CHECK(report.complete);
  KB_CHECK_EQ(report.stages, 1U);
  KB_CHECK_EQ(report.kinds, kb::handlers::stage::kIdentify | kb::handlers::stage::kConfigure);
  const int conversions = r.sensor.state.transfers;
  KB_CHECK(conversions > 0); // the first reading is the start-up's plausibility check
  KB_CHECK(r.io.EnsureStarted(kWait).IsOk()); // idempotent, no I/O
  KB_CHECK_EQ(r.sensor.state.transfers, conversions);
  const auto snap = r.io.Sample(kWait);
  KB_REQUIRE(snap.IsOk());
  KB_CHECK(Near(snap.Value().value.temperature_mc, 25'000, 500));
  KB_CHECK(Near(static_cast<int32_t>(snap.Value().value.output_mv), 750, 5));
  KB_CHECK(r.io.Latest().IsOk());
  KB_CHECK_EQ(r.io.Health().Value().starts, 1U);
}

KB_TEST("mcp9700: calls before a start convert nothing and return NotReady") {
  GuardedRig r{Defaults()};
  KB_CHECK(binding::ReadRawMv(r.io, kWait).Error() == kb::Status::NotReady);
  KB_CHECK(binding::ReadTemperature(r.io, kWait).Error() == kb::Status::NotReady);
  KB_CHECK(r.io.Latest().Error() == kb::Status::NotReady);
  KB_CHECK_EQ(r.sensor.state.transfers, 0);
}

KB_TEST("mcp9700: a grounded output fails the start with Fault and the driver's code; a retry "
        "starts again") {
  GuardedRig r{Defaults()};
  r.sensor.state.fault = mcp9700_test::Fault::StuckLow;
  const kb::Result<void> first = r.io.EnsureStarted(kWait);
  KB_CHECK(first.Error() == kb::Status::Fault);
  KB_CHECK_EQ(first.Detail(), kb::handlers::DetailOf(mcp9700::Error::DeviceIdMismatch));
  const kb::handlers::HealthRecord h = r.io.Health().Value();
  KB_CHECK(h.state == HealthState::Faulted);
  KB_CHECK_EQ(h.start_failures, 1U);
  const kb::handlers::StartupReport rep = r.io.Startup().Value();
  KB_CHECK(!rep.complete);
  KB_CHECK_EQ(rep.stage, 0U);
  KB_CHECK_EQ(rep.detail, kb::handlers::DetailOf(mcp9700::Error::DeviceIdMismatch));
  r.sensor.state.fault = mcp9700_test::Fault::None;
  KB_CHECK(r.io.EnsureStarted(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  KB_CHECK_EQ(r.io.Startup().Value().runs, 2U);
}

KB_TEST("mcp9700: the supply pin powers the sensor at start and off at Stop; a pin failure is "
        "BusError") {
  kb_it::Timing t{};
  ModelSensor sensor{};
  ModelAdc adc{sensor};
  SupplyPin supply{sensor};
  GuardedBus domain{adc};
  binding::Config cfg = Defaults();
  cfg.enable.pin = kb::iface::GpioOutputRef{supply};
  GuardedIo io{domain, adc, t.timing, cfg};
  KB_REQUIRE(domain.Create().IsOk());
  KB_REQUIRE(io.Create().IsOk());
  supply.pin.FailNext(kb::Status::BusError);
  const kb::Result<void> bad = io.EnsureStarted(kWait);
  KB_CHECK(bad.Error() == kb::Status::BusError);
  KB_CHECK_EQ(bad.Detail(), kb::handlers::DetailOf(mcp9700::Error::Bus));
  KB_REQUIRE(io.EnsureStarted(kWait).IsOk()); // the driver waited the turn-on time of the model
  KB_CHECK(supply.pin.LevelNow() == kb::iface::Level::High);
  KB_CHECK(sensor.state.powered);
  KB_CHECK(binding::ReadTemperature(io, kWait).IsOk());
  KB_CHECK(io.Stop(kWait).IsOk());
  KB_CHECK(supply.pin.LevelNow() == kb::iface::Level::Low);
  KB_CHECK(kb_it::StateOf(io) == HealthState::Unknown);
}

KB_TEST("mcp9700: the commands reach the model") {
  GuardedRig r{Defaults()};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  const kb::Result<uint32_t> raw = binding::ReadRawMv(r.io, kWait);
  KB_REQUIRE(raw.IsOk());
  KB_CHECK(Near(static_cast<int32_t>(raw.Value()), 750, 5));
  r.sensor.state.temperature_mc = 60'000;
  binding::Command c{};
  c.op = binding::Op::ReadMeasurement;
  const kb::Result<binding::Reply> m = r.io.Request(c, kWait);
  KB_REQUIRE(m.IsOk());
  KB_CHECK(Near(m.Value().reading.temperature_mc, 60'000, 500));
  const kb::Result<int32_t> mc = binding::ReadTemperature(r.io, kWait);
  KB_REQUIRE(mc.IsOk());
  KB_CHECK(Near(mc.Value(), 60'000, 500));
}

KB_TEST("mcp9700: ADC failures degrade, implausible readings fault at the threshold, Recover() "
        "restores Ready") {
  GuardedRig r{Defaults()};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  r.sensor.state.fail_transfer = r.sensor.state.transfers; // the next conversion fails
  const kb::Result<int32_t> bad = binding::ReadTemperature(r.io, kWait);
  KB_CHECK(bad.Error() == kb::Status::BusError);
  KB_CHECK_EQ(bad.Detail(), kb::handlers::DetailOf(mcp9700::Error::Bus));
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Degraded);
  KB_CHECK(binding::ReadTemperature(r.io, kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  r.sensor.state.fault = mcp9700_test::Fault::FloatHigh; // the ADC full scale: saturated
  const kb::Result<int32_t> high = binding::ReadTemperature(r.io, kWait);
  KB_CHECK(high.Error() == kb::Status::OutOfRange);
  KB_CHECK_EQ(high.Detail(), kb::handlers::DetailOf(mcp9700::Error::Saturated));
  KB_CHECK(!binding::ReadTemperature(r.io, kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Faulted);
  const int before = r.sensor.state.transfers;
  KB_CHECK(binding::ReadRawMv(r.io, kWait).Error() == kb::Status::NotReady);
  KB_CHECK_EQ(r.sensor.state.transfers, before); // reads never restart a faulted sensor
  r.sensor.state.fault = mcp9700_test::Fault::None;
  KB_CHECK(r.io.RequestRecovery(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  KB_CHECK_EQ(r.io.Health().Value().recoveries, 1U);
}

KB_TEST("mcp9700: NotInitialized is the fatal error; range errors are not") {
  kb_it::Timing t{};
  ModelSensor sensor{};
  ModelAdc adc{sensor};
  const Handler h{adc, t.timing, Defaults()};
  KB_CHECK(h.IsFatal(kb::Status::NotReady, kb::handlers::DetailOf(mcp9700::Error::NotInitialized)));
  KB_CHECK(!h.IsFatal(kb::Status::OutOfRange, kb::handlers::DetailOf(mcp9700::Error::OutputHigh)));
  KB_CHECK(!h.IsFatal(kb::Status::BusError, kb::handlers::DetailOf(mcp9700::Error::Bus)));
}

KB_TEST("mcp9700: owner model: the owner task starts, samples and serves requests") {
  kb_it::Timing t{};
  ModelSensor sensor{};
  ModelAdc adc{sensor};
  OwnerBus domain{adc};
  binding::Config cfg = Defaults();
  cfg.handler.sample_period = kb::Milliseconds{5U};
  OwnedIo io{domain, adc, t.timing, cfg};
  KB_REQUIRE(io.Create().IsOk());
  const kb_it::OwnerLoop<OwnerBus, OwnedIo> owner{domain, io};
  KB_REQUIRE(io.EnsureStarted(kWait).IsOk());
  sensor.state.temperature_mc = -10'000;
  const kb::Result<int32_t> mc = binding::ReadTemperature(io, kWait);
  KB_REQUIRE(mc.IsOk());
  KB_CHECK(Near(mc.Value(), -10'000, 500));
  KB_CHECK(binding::ReadRawMv(io, kWait).IsOk());
  KB_CHECK(io.Latest().IsOk());
  KB_CHECK(io.Liveness().Beats() > 0U);
  KB_CHECK(io.Stop(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(io) == HealthState::Unknown);
}

} // namespace
