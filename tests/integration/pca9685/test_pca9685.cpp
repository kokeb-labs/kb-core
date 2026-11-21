// pca9685 through kb::handlers::pca9685: start-up, the app API, the OE pin, error mapping to the
// health record, recovery, and both thread models, over a kb::iface::I2cBus backed by the driver's
// own register model (components/drivers/driver-pca9685/tests/mock_pca9685_bus.hpp), whose time
// follows the RTOS clock. SPDX-License-Identifier: MIT Copyright (c) 2026 The kb-core authors
#include <cstdint>
#include <span>

#include "binding_support.hpp"
#include "fixture.hpp"
#include "kb/handlers/bindings/pca9685.hpp"
#include "kb/interfaces/host/fake_pins.hpp"
#include "kb_test.hpp"
#include "mock_pca9685_bus.hpp"

namespace {

using kb::handlers::HealthState;
namespace binding = kb::handlers::pca9685;

/// kb::iface::I2cBus over the register model and its fault injection.
class ModelBus {
public:
  pca9685_test::MockBusState state{};

  [[nodiscard]] kb::Status EnsureReady() noexcept {
    return state.bus_up_ok ? kb::Status::Ok : kb::Status::NotReady;
  }
  [[nodiscard]] kb::Status Write(uint8_t address, std::span<const uint8_t> tx) noexcept {
    elapse();
    return outcome(state.DoWrite(address, tx.data(), tx.size()));
  }
  [[nodiscard]] kb::Status WriteRead(uint8_t address, std::span<const uint8_t> tx,
                                     std::span<uint8_t> rx) noexcept {
    elapse();
    return outcome(state.DoWriteRead(address, tx.data(), tx.size(), rx.data(), rx.size()));
  }
  [[nodiscard]] kb::Status Read(uint8_t address, std::span<uint8_t> rx) noexcept {
    static_cast<void>(address);
    static_cast<void>(rx);
    last_ = kb::iface::I2cFailure::Bus;
    return kb::Status::Unsupported; // the driver never uses a bare read
  }
  [[nodiscard]] kb::iface::I2cFailure LastFailure() const noexcept {
    return last_;
  }

private:
  void elapse() noexcept {
    const uint64_t now = kb::rtos::Now().Us();
    state.chip.Elapse(static_cast<uint32_t>(now - last_us_));
    last_us_ = now;
  }
  [[nodiscard]] kb::Status outcome(bool ok) noexcept {
    kb::Status s = kb::Status::Ok;
    last_ = kb::iface::I2cFailure::None;
    if (!ok) {
      switch (state.last_failure) {
      case pca9685::I2cFailure::AddressNack:
        last_ = kb::iface::I2cFailure::AddressNack;
        s = kb::Status::NoDevice;
        break;
      case pca9685::I2cFailure::DataNack:
        last_ = kb::iface::I2cFailure::DataNack;
        s = kb::Status::Nack;
        break;
      case pca9685::I2cFailure::Other:
        last_ = kb::iface::I2cFailure::Bus;
        s = kb::Status::BusError;
        break;
      }
    }
    return s;
  }
  kb::iface::I2cFailure last_{kb::iface::I2cFailure::None};
  uint64_t last_us_{kb::rtos::Now().Us()};
};
static_assert(kb::iface::I2cBus<ModelBus>);

/// The OE pin: a host fake (fault injection) whose level reaches the model.
class OePin {
public:
  explicit OePin(ModelBus& bus) noexcept : bus_(bus) {
    bus_.state.chip.oe_high = true; // pulled up until the MCU drives it
  }
  kb::iface::host::FakeGpioPin pin{};
  [[nodiscard]] kb::Status Set(kb::iface::Level level) noexcept {
    const kb::Status s = pin.Set(level);
    if (s == kb::Status::Ok) {
      bus_.state.chip.oe_high = level == kb::iface::Level::High;
    }
    return s;
  }
  [[nodiscard]] kb::Status Toggle() noexcept {
    return Set((pin.LevelNow() == kb::iface::Level::High) ? kb::iface::Level::Low
                                                          : kb::iface::Level::High);
  }

private:
  ModelBus& bus_;
};
static_assert(kb::iface::GpioOutput<OePin>);

using Handler = binding::Handler<ModelBus>;
using OeHandler = binding::Handler<ModelBus, true>;
using GuardedBus = kb::handlers::BusDomain<ModelBus, kb::handlers::ThreadModel::Guarded>;
using OwnerBus = kb::handlers::BusDomain<ModelBus, kb::handlers::ThreadModel::Owner>;
using GuardedIo = kb::handlers::Guarded<Handler, GuardedBus>;
using OeIo = kb::handlers::Guarded<OeHandler, GuardedBus>;
using OwnedIo = kb::handlers::OwnerAccess<Handler, OwnerBus>;

constexpr kb::Milliseconds kWait{200U};

static_assert(binding::ToStatus(pca9685::Error::DeviceReset) == kb::Status::NotReady);
static_assert(binding::ToStatus(pca9685::Error::NoDevice) == kb::Status::NoDevice);
static_assert(binding::ToStatus(pca9685::Error::InvalidReply) == kb::Status::BusError);

binding::Config Defaults() {
  binding::Config c{};
  c.handler.health.fault_after = 2U;
  c.handler.recovery.backoff = kb::Milliseconds{1U};
  return c;
}

/// A guarded handler on its own bus, created.
struct GuardedRig {
  kb_it::Timing t{};
  ModelBus bus{};
  GuardedBus domain{bus};
  GuardedIo io;
  explicit GuardedRig(const binding::Config& cfg) : io(domain, bus, t.timing, cfg) {
    static_cast<void>(domain.Create());
    static_cast<void>(io.Create());
  }
};

pca9685::Pwm Channel(const ModelBus& bus, uint32_t ch) {
  return pca9685_test::Decode(pca9685_test::Stored(bus.state.chip, ch));
}

KB_TEST("pca9685: guarded start runs the one sync stage, every output off, the chip awake") {
  GuardedRig r{Defaults()};
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Unknown);
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  const kb::handlers::StartupReport report = r.io.Startup().Value();
  KB_CHECK(report.complete);
  KB_CHECK_EQ(report.stages, 1U);
  KB_CHECK_EQ(report.kinds, kb::handlers::stage::kIdentify | kb::handlers::stage::kConfigure);
  KB_CHECK(!r.bus.state.chip.Asleep());
  KB_CHECK(Channel(r.bus, 0U).full_off);
  const int transfers = r.bus.state.transfers;
  KB_CHECK(r.io.EnsureStarted(kWait).IsOk()); // idempotent, no I/O
  KB_CHECK_EQ(r.bus.state.transfers, transfers);
}

KB_TEST("pca9685: calls before a start send nothing and return NotReady") {
  GuardedRig r{Defaults()};
  KB_CHECK(binding::SetDuty(r.io, 0U, 100U, kWait).Error() == kb::Status::NotReady);
  KB_CHECK(binding::SetAllOff(r.io, kWait).Error() == kb::Status::NotReady);
  KB_CHECK(r.io.Sample(kWait).Error() == kb::Status::NotReady);
  KB_CHECK_EQ(r.bus.state.transfers, 0);
}

KB_TEST("pca9685: an absent chip fails the start with NoDevice and the driver's code; a retry "
        "starts again") {
  GuardedRig r{Defaults()};
  r.bus.state.fail_from = 0; // nothing acknowledges the address
  const kb::Result<void> first = r.io.EnsureStarted(kWait);
  KB_CHECK(first.Error() == kb::Status::NoDevice);
  KB_CHECK_EQ(first.Detail(), kb::handlers::DetailOf(pca9685::Error::NoDevice));
  const kb::handlers::HealthRecord h = r.io.Health().Value();
  KB_CHECK(h.state == HealthState::Faulted);
  KB_CHECK_EQ(h.start_failures, 1U);
  KB_CHECK(!r.io.Startup().Value().complete);
  r.bus.state.fail_from = -1;
  KB_CHECK(r.io.EnsureStarted(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
}

KB_TEST(
    "pca9685: duty, pulse width, read-back, frequency and switch-offs reach the register model") {
  GuardedRig r{Defaults()};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  KB_REQUIRE(binding::SetDuty(r.io, 0U, 2048U, kWait).IsOk());
  KB_CHECK_EQ(Channel(r.bus, 0U).off_count, 2048U);
  KB_REQUIRE(binding::SetPwmFrequency(r.io, 50U, kWait).IsOk());
  const kb::Result<uint32_t> width = binding::SetPulseWidthUs(r.io, 1U, 1500U, kWait);
  KB_REQUIRE(width.IsOk());
  KB_CHECK((width.Value() > 1'480'000U) && (width.Value() < 1'520'000U)); // nanoseconds
  binding::Command c{};
  c.op = binding::Op::ReadChannel;
  c.channel = 1U;
  const auto read = r.io.Request(c, kWait);
  KB_REQUIRE(read.IsOk());
  KB_CHECK(read.Value().pwm == Channel(r.bus, 1U));
  const auto snap = r.io.Sample(kWait);
  KB_REQUIRE(snap.IsOk());
  KB_CHECK((snap.Value().value.frequency_hz >= 49U) && (snap.Value().value.frequency_hz <= 51U));
  KB_CHECK_EQ(snap.Value().value.uncertain, 0U);
  KB_CHECK(binding::SetChannelOff(r.io, 0U, kWait).IsOk());
  KB_CHECK(Channel(r.bus, 0U).full_off);
  KB_CHECK(binding::SetAllOff(r.io, kWait).IsOk());
  KB_CHECK(Channel(r.bus, 1U).full_off);
}

KB_TEST("pca9685: with OE the start enables the outputs and DisableOutputs drives OE high; without "
        "OE it is Unsupported") {
  kb_it::Timing t{};
  ModelBus bus{};
  OePin oe{bus};
  GuardedBus domain{bus};
  binding::Config cfg = Defaults();
  cfg.output_enable.pin = kb::iface::GpioOutputRef{oe};
  OeIo io{domain, bus, t.timing, cfg};
  KB_REQUIRE(domain.Create().IsOk());
  KB_REQUIRE(io.Create().IsOk());
  KB_REQUIRE(io.EnsureStarted(kWait).IsOk());
  KB_CHECK(oe.pin.LevelNow() == kb::iface::Level::Low); // asserted: outputs enabled
  KB_CHECK(binding::DisableOutputs(io, kWait).IsOk());
  KB_CHECK(bus.state.chip.oe_high);
  KB_CHECK(binding::EnableOutputs(io, kWait).IsOk());
  KB_CHECK(!bus.state.chip.oe_high);
  oe.pin.FailNext(kb::Status::BusError);
  const kb::Result<void> bad = binding::DisableOutputs(io, kWait);
  KB_CHECK(bad.Error() == kb::Status::BusError);
  KB_CHECK_EQ(bad.Detail(), kb::handlers::DetailOf(pca9685::Error::Bus));
  KB_CHECK(io.Stop(kWait).IsOk());
  KB_CHECK(bus.state.chip.oe_high); // stopped: OE high first

  GuardedRig plain{Defaults()};
  KB_REQUIRE(plain.io.EnsureStarted(kWait).IsOk());
  const kb::Result<void> none = binding::DisableOutputs(plain.io, kWait);
  KB_CHECK(none.Error() == kb::Status::Unsupported);
  KB_CHECK_EQ(none.Detail(), kb::handlers::DetailOf(pca9685::Error::Unsupported));
}

KB_TEST("pca9685: bus failures degrade, the threshold faults, Recover() restores Ready") {
  GuardedRig r{Defaults()};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  r.bus.state.nack_transfer = r.bus.state.transfers; // a data byte refused
  r.bus.state.nack_after = 1U;
  const kb::Result<void> bad = binding::SetDuty(r.io, 2U, 100U, kWait);
  KB_CHECK(bad.Error() == kb::Status::BusError);
  KB_CHECK_EQ(bad.Detail(), kb::handlers::DetailOf(pca9685::Error::Bus));
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Degraded);
  KB_CHECK(binding::SetDuty(r.io, 2U, 100U, kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  r.bus.state.fail_from = r.bus.state.transfers;
  KB_CHECK(!binding::SetDuty(r.io, 2U, 200U, kWait).IsOk());
  KB_CHECK(!binding::SetDuty(r.io, 2U, 200U, kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Faulted);
  const int before = r.bus.state.transfers;
  KB_CHECK(binding::SetDuty(r.io, 2U, 200U, kWait).Error() == kb::Status::NotReady);
  KB_CHECK_EQ(r.bus.state.transfers, before);
  r.bus.state.fail_from = -1;
  KB_CHECK(r.io.RequestRecovery(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  KB_CHECK_EQ(r.io.Health().Value().recoveries, 1U);
}

KB_TEST("pca9685: a power-on reset of the chip is fatal at once; recovery programs it again") {
  GuardedRig r{Defaults()};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  r.bus.state.chip.PowerOnReset();
  const auto res = r.io.Sample(kWait); // the sample is the reset check
  KB_CHECK(res.Error() == kb::Status::NotReady);
  KB_CHECK_EQ(res.Detail(), kb::handlers::DetailOf(pca9685::Error::DeviceReset));
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Faulted);
  KB_CHECK(r.io.RequestRecovery(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  KB_CHECK(!r.bus.state.chip.Asleep());
}

KB_TEST("pca9685: owner model: the owner task starts, checks for resets and serves requests") {
  kb_it::Timing t{};
  ModelBus bus{};
  OwnerBus domain{bus};
  binding::Config cfg = Defaults();
  cfg.handler.sample_period = kb::Milliseconds{2U};
  OwnedIo io{domain, bus, t.timing, cfg};
  KB_REQUIRE(io.Create().IsOk());
  const kb_it::OwnerLoop<OwnerBus, OwnedIo> owner{domain, io};
  KB_REQUIRE(io.EnsureStarted(kWait).IsOk());
  KB_CHECK(binding::SetDuty(io, 3U, 1000U, kWait).IsOk());
  const auto snap = io.Sample(kWait);
  KB_REQUIRE(snap.IsOk());
  KB_CHECK((snap.Value().value.frequency_hz >= 196U) &&
           (snap.Value().value.frequency_hz <= 198U)); // PRE_SCALE 30: 196.9 Hz
  KB_CHECK(io.Liveness().Beats() > 0U);
  KB_CHECK(io.Stop(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(io) == HealthState::Unknown);
}

} // namespace
