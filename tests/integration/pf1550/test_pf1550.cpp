// pf1550 through kb::handlers::pf1550: the two start-up stages, the status snapshot, the app API,
// the STANDBY and INTB pins, error mapping to the health record, recovery, and both thread models,
// over a kb::iface::I2cBus backed by the driver's own register model
// (components/drivers/driver-pf1550/tests/mock_pf1550_bus.hpp), whose time follows the RTOS clock.
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include <cstddef>
#include <cstdint>
#include <span>

#include "binding_support.hpp"
#include "kb/handlers/bindings/pf1550.hpp"
#include "kb/interfaces/host/fake_pins.hpp"
#include "kb_test.hpp"
#include "mock_pf1550_bus.hpp"

namespace {

using kb::handlers::HealthState;
namespace pfh = kb::handlers::pf1550;

/// kb::iface::I2cBus over the driver's register model and its fault injection; model time follows
/// kb::rtos::Now().
class ModelBus {
public:
  pf1550_test::MockBusState state;

  [[nodiscard]] kb::Status EnsureReady() noexcept {
    return state.bus_up_ok ? kb::Status::Ok : kb::Status::NotReady;
  }
  [[nodiscard]] kb::Status Write(uint8_t address, std::span<const uint8_t> tx) noexcept {
    sync();
    return outcome(state.DoWrite(address, tx.data(), tx.size()));
  }
  [[nodiscard]] kb::Status WriteRead(uint8_t address, std::span<const uint8_t> tx,
                                     std::span<uint8_t> rx) noexcept {
    sync();
    return outcome(state.DoWriteRead(address, tx.data(), tx.size(), rx.data(), rx.size()));
  }
  [[nodiscard]] kb::Status Read(uint8_t address, std::span<uint8_t> rx) noexcept {
    static_cast<void>(address);
    static_cast<void>(rx);
    return kb::Status::Unsupported; // the driver never uses a bare read
  }
  [[nodiscard]] kb::iface::I2cFailure LastFailure() const noexcept {
    return last_;
  }

private:
  void sync() noexcept {
    const uint64_t now = kb::rtos::Now().Us();
    state.chip.Elapse(static_cast<uint32_t>(now - last_us_));
    last_us_ = now;
  }
  [[nodiscard]] kb::Status outcome(bool ok) noexcept {
    kb::Status s = kb::Status::Ok;
    last_ = kb::iface::I2cFailure::None;
    if (!ok) {
      if (state.last_failure == pf1550::I2cFailure::AddressNack) {
        last_ = kb::iface::I2cFailure::AddressNack;
        s = kb::Status::NoDevice;
      } else {
        last_ = kb::iface::I2cFailure::DataNack;
        s = kb::Status::Nack;
      }
    }
    return s;
  }
  uint64_t last_us_{kb::rtos::Now().Us()};
  kb::iface::I2cFailure last_{kb::iface::I2cFailure::None};
};
static_assert(kb::iface::I2cBus<ModelBus>);

/// INTB as a kb::iface::GpioInput: the model's open-drain output, low when an unmasked source is
/// set.
struct IntbPin {
  pf1550_test::Pf1550Model* chip{nullptr};
  [[nodiscard]] kb::Status Get(kb::iface::Level& out) noexcept {
    out = chip->IntbAsserted() ? kb::iface::Level::Low : kb::iface::Level::High;
    return kb::Status::Ok;
  }
};
static_assert(kb::iface::GpioInput<IntbPin>);

using Handler = pfh::Handler<ModelBus>;
using GuardedBus = kb::handlers::BusDomain<ModelBus, kb::handlers::ThreadModel::Guarded>;
using OwnerBus = kb::handlers::BusDomain<ModelBus, kb::handlers::ThreadModel::Owner>;
using GuardedIo = kb::handlers::Guarded<Handler, GuardedBus>;
using OwnedIo = kb::handlers::OwnerAccess<Handler, OwnerBus>;

constexpr kb::Milliseconds kWait{200U};

/// A 4.2 V cell charged at up to 500 mA from a 1 A supply, rail windows, the charger written, every
/// source unmasked (INTB follows the status bits), SW1 a DVS buck (option A2).
pfh::Config ChargerConfig() {
  pfh::Config c{};
  c.chip.limits = pf1550::SingleCellLimits();
  c.chip.limits.coin_cell_max_mv = 3300;
  c.chip.limits.rail[static_cast<std::size_t>(pf1550::Rail::Sw1)] = {1000, 1200, 800};
  c.chip.limits.rail[static_cast<std::size_t>(pf1550::Rail::Sw2)] = {1100, 1500, 800};
  c.chip.limits.rail[static_cast<std::size_t>(pf1550::Rail::Sw3)] = {1800, 3300, 500};
  c.chip.limits.rail[static_cast<std::size_t>(pf1550::Rail::Ldo1)] = {1700, 3300, 100};
  c.chip.limits.rail[static_cast<std::size_t>(pf1550::Rail::Ldo2)] = {1800, 3300, 100};
  c.chip.limits.rail[static_cast<std::size_t>(pf1550::Rail::Ldo3)] = {1700, 3300, 100};
  c.chip.limits.rail[static_cast<std::size_t>(pf1550::Rail::VrefDdr)] = {600, 900, 5};
  c.chip.sw1.dvs = pf1550::Dvs::Enabled;
  c.chip.charger.apply = true;
  c.chip.charger.mode = pf1550::ChargerMode::LinearOnly;
  c.chip.charger.charge_voltage_mv = 4200;
  c.chip.charger.charge_current_ma = 500;
  c.chip.charger.input_limit_ma = 1000;
  c.chip.apply_irq_mask = true;
  c.chip.irq_mask = pf1550::IrqMask{{0, 0, 0, 0, 0, 0, 0, 0}};
  c.handler.health.fault_after = 2U;
  c.handler.recovery.backoff = kb::Milliseconds{1U};
  return c;
}

/// A guarded handler on its own bus with the STANDBY and INTB pins, created; the chip on option A2
/// with VBUS valid.
struct GuardedRig {
  kb_it::Timing t{};
  ModelBus bus{};
  kb::iface::host::FakeGpioPin standby{};
  IntbPin intb{};
  GuardedBus domain{bus};
  GuardedIo io;
  explicit GuardedRig(pfh::Config cfg)
      : io(domain, bus, t.timing, WithPins(cfg, standby, intb, bus)) {
    bus.state.chip.Reset(2);
    bus.state.chip.env.vbus_valid = true;
    static_cast<void>(domain.Create());
    static_cast<void>(io.Create());
  }
  static pfh::Config WithPins(pfh::Config cfg, kb::iface::host::FakeGpioPin& standby, IntbPin& intb,
                              ModelBus& bus) {
    intb.chip = &bus.state.chip;
    cfg.standby.pin = kb::iface::GpioOutputRef{standby};
    cfg.intb.pin = kb::iface::GpioInputRef{intb};
    cfg.intb.active_low = true;
    return cfg;
  }
};

KB_TEST("pf1550: guarded start runs Identify then Configure, the charger reaches the chip") {
  GuardedRig r{ChargerConfig()};
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Unknown);
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  const kb::handlers::StartupReport rep = r.io.Startup().Value();
  KB_CHECK(rep.complete);
  KB_CHECK_EQ(rep.stages, 2U);
  KB_CHECK_EQ(rep.kinds, kb::handlers::stage::kConfigure);
  KB_CHECK_EQ(r.bus.state.chip.Peek(0x89) & 3U, 1U); // CHG_OPER: linear only, as configured
  KB_CHECK_EQ(r.bus.state.chip.AppliedCvMv(), 4200U);
  const int before = r.bus.state.transfers;
  KB_CHECK(r.io.EnsureStarted(kWait).IsOk()); // idempotent, no traffic
  KB_CHECK_EQ(r.bus.state.transfers, before);
}

KB_TEST("pf1550: calls before a start send nothing and return NotReady") {
  GuardedRig r{ChargerConfig()};
  KB_CHECK(pfh::SetChargerMode(r.io, pf1550::ChargerMode::Charge, kWait).Error() ==
           kb::Status::NotReady);
  KB_CHECK(pfh::ServiceInterrupts(r.io, kWait).Error() == kb::Status::NotReady);
  KB_CHECK(r.io.Sample(kWait).Error() == kb::Status::NotReady);
  KB_CHECK_EQ(r.bus.state.transfers, 0);
}

KB_TEST("pf1550: an absent chip fails Identify with NoDevice; a wrong ID fails with Fault; a retry "
        "starts again") {
  GuardedRig r{ChargerConfig()};
  r.bus.state.address = 0x09U; // the chip answers elsewhere: every address NACKed
  const kb::Result<void> first = r.io.EnsureStarted(kWait);
  KB_CHECK(first.Error() == kb::Status::NoDevice);
  KB_CHECK_EQ(first.Detail(), kb::handlers::DetailOf(pf1550::Error::NoDevice));
  KB_CHECK_EQ(r.io.Startup().Value().stage, 0U);
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Faulted);
  r.bus.state.address = pf1550::kI2cAddress;
  r.bus.state.stuck_addr = 0x00U; // DEVICE_ID
  r.bus.state.stuck_value = 0x55U;
  const kb::Result<void> second = r.io.EnsureStarted(kWait);
  KB_CHECK(second.Error() == kb::Status::Fault);
  KB_CHECK_EQ(second.Detail(), kb::handlers::DetailOf(pf1550::Error::DeviceIdMismatch));
  r.bus.state.stuck_addr = 0x100U;
  KB_CHECK(r.io.EnsureStarted(kWait).IsOk());
  KB_CHECK_EQ(r.io.Health().Value().start_failures, 2U);
}

KB_TEST("pf1550: the snapshot reads the charger, the power state and INTB") {
  GuardedRig r{ChargerConfig()};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  const auto snap = r.io.Sample(kWait);
  KB_REQUIRE(snap.IsOk());
  const pfh::Snapshot& s = snap.Value().value;
  KB_CHECK(s.state == pf1550::PowerState::Run);
  KB_CHECK(s.charger.vbus.valid);
  KB_CHECK(s.charger.consistent);
  KB_CHECK(s.has_intb);
  KB_CHECK(!s.intb_asserted);
  r.bus.state.chip.Raise(0x18, 1); // LDO2_FAULTI
  const auto again = r.io.Sample(kWait);
  KB_REQUIRE(again.IsOk());
  KB_CHECK(again.Value().value.intb_asserted);
  KB_CHECK(r.io.Latest().Value().value.intb_asserted);
}

KB_TEST("pf1550: charger, rails, standby, interrupts and the self check through the app API") {
  GuardedRig r{ChargerConfig()};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  KB_CHECK(pfh::SetChargerMode(r.io, pf1550::ChargerMode::Charge, kWait).IsOk());
  KB_CHECK_EQ(r.bus.state.chip.Peek(0x89) & 3U, 2U);
  KB_CHECK(pfh::ServiceChargerWatchdog(r.io, kWait).IsOk());
  KB_CHECK(pfh::RestartCharger(r.io, kWait).IsOk());
  KB_CHECK(
      pfh::SetRailVoltage(r.io, pf1550::Rail::Ldo1, pf1550::FromMillivolts(2800U), kWait).IsOk());
  KB_CHECK_EQ(r.bus.state.chip.Peek(0x4C), 26U);
  KB_CHECK(pfh::SetRailVoltage(r.io, pf1550::Rail::Sw1, pf1550::FromMillivolts(1100U), kWait)
               .IsOk()); // DVS wait
  const auto rail = pfh::ReadRail(r.io, pf1550::Rail::Sw1, kWait);
  KB_REQUIRE(rail.IsOk());
  KB_CHECK_EQ(rail.Value().volt.value, 1100000U);
  KB_CHECK(pfh::RequestStandby(r.io, true, kWait).IsOk());
  KB_CHECK(r.standby.LevelNow() == kb::iface::Level::High);
  KB_CHECK(pfh::RequestStandby(r.io, false, kWait).IsOk());
  KB_CHECK(r.standby.LevelNow() == kb::iface::Level::Low);
  r.bus.state.chip.Raise(0x28, 1); // PWRDN_I
  const auto events = pfh::ServiceInterrupts(r.io, kWait);
  KB_REQUIRE(events.IsOk());
  KB_CHECK(pf1550::Has(events.Value(), pf1550::IrqSource::PowerDown));
  KB_CHECK(!r.bus.state.chip.IntbAsserted());
  const auto check = pfh::SelfCheck(r.io, kWait);
  KB_REQUIRE(check.IsOk());
  KB_CHECK(check.Value().identity_ok && check.Value().state_run);
  pfh::Command reg{};
  reg.op = pfh::Op::ReadRegister;
  reg.address = 0x00U;
  const auto id = r.io.Request(reg, kWait);
  KB_REQUIRE(id.IsOk());
  KB_CHECK_EQ(id.Value().value, 0x7CU);
}

KB_TEST(
    "pf1550: refusals and bus failures degrade, the threshold faults, Recover() restores Ready") {
  GuardedRig r{ChargerConfig()};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  const auto limit =
      pfh::SetRailVoltage(r.io, pf1550::Rail::Sw1, pf1550::FromMillivolts(1300U), kWait);
  KB_CHECK(limit.Error() == kb::Status::OutOfRange);
  KB_CHECK_EQ(limit.Detail(), kb::handlers::DetailOf(pf1550::Error::OutOfLimits));
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Degraded);
  KB_CHECK(r.io.Sample(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  r.bus.state.fail_from = r.bus.state.transfers; // every transfer is NACKed from now on
  const auto bad = r.io.Sample(kWait);
  KB_CHECK(bad.Error() == kb::Status::BusError);
  KB_CHECK_EQ(bad.Detail(), kb::handlers::DetailOf(pf1550::Error::Bus));
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Degraded);
  KB_CHECK(!r.io.Sample(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Faulted);
  const int before = r.bus.state.transfers;
  KB_CHECK(pfh::ServiceChargerWatchdog(r.io, kWait).Error() == kb::Status::NotReady);
  KB_CHECK_EQ(r.bus.state.transfers, before); // a faulted handler sends nothing
  r.bus.state.fail_from = -1;
  KB_CHECK(r.io.RequestRecovery(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  KB_CHECK_EQ(r.io.Health().Value().recoveries, 1U);
}

KB_TEST("pf1550: a chip reset seen by a call is fatal at once; recovery writes the configuration "
        "again") {
  GuardedRig r{ChargerConfig()};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  KB_CHECK(pfh::SetChargerMode(r.io, pf1550::ChargerMode::Charge, kWait).IsOk());
  r.bus.state.chip.Reset(2); // a power loss: every register back at its start-up value
  r.bus.state.chip.env.vbus_valid = true;
  const auto res = pfh::ServiceChargerWatchdog(r.io, kWait);
  KB_CHECK(res.Error() == kb::Status::NotReady);
  KB_CHECK_EQ(res.Detail(), kb::handlers::DetailOf(pf1550::Error::DeviceReset));
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Faulted);
  KB_CHECK(r.io.RequestRecovery(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  KB_CHECK_EQ(r.bus.state.chip.Peek(0x89) & 3U, 1U); // the configured charger mode is written again
}

KB_TEST("pf1550: without pins the handler still starts; standby is Unsupported and INTB is not "
        "reported") {
  kb_it::Timing t{};
  ModelBus bus{};
  bus.state.chip.Reset(2);
  bus.state.chip.env.vbus_valid = true;
  GuardedBus domain{bus};
  GuardedIo io{domain, bus, t.timing, ChargerConfig()};
  KB_REQUIRE(domain.Create().IsOk());
  KB_REQUIRE(io.Create().IsOk());
  KB_REQUIRE(io.EnsureStarted(kWait).IsOk());
  const auto snap = io.Sample(kWait);
  KB_REQUIRE(snap.IsOk());
  KB_CHECK(!snap.Value().value.has_intb);
  KB_CHECK(pfh::RequestStandby(io, true, kWait).Error() == kb::Status::BusError);
  bus.state.chip.Raise(0x28, 1);
  const auto events = pfh::ServiceInterrupts(io, kWait); // without INTB every group is read
  KB_REQUIRE(events.IsOk());
  KB_CHECK(pf1550::Has(events.Value(), pf1550::IrqSource::PowerDown));
}

KB_TEST("pf1550: owner model: the owner task starts, samples and serves requests") {
  kb_it::Timing t{};
  ModelBus bus{};
  bus.state.chip.Reset(2);
  bus.state.chip.env.vbus_valid = true;
  OwnerBus domain{bus};
  pfh::Config cfg = ChargerConfig();
  cfg.handler.sample_period = kb::Milliseconds{5U};
  OwnedIo io{domain, bus, t.timing, cfg};
  KB_REQUIRE(io.Create().IsOk());
  const kb_it::OwnerLoop<OwnerBus, OwnedIo> owner{domain, io};
  KB_REQUIRE(io.EnsureStarted(kWait).IsOk());
  KB_CHECK(pfh::SetChargerMode(io, pf1550::ChargerMode::Charge, kWait).IsOk());
  KB_CHECK_EQ(bus.state.chip.Peek(0x89) & 3U, 2U);
  const auto snap = io.Sample(kWait);
  KB_REQUIRE(snap.IsOk());
  KB_CHECK(snap.Value().value.state == pf1550::PowerState::Run);
  KB_CHECK(io.Liveness().Beats() > 0U);
  KB_CHECK(io.Stop(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(io) == HealthState::Unknown);
}

} // namespace
