// tle92466ed through kb::handlers::tle92466ed: start-up, the app API, error mapping to the health
// record, what a fault does to the outputs, recovery, the SPI watchdog under an owner task, and
// both thread models, over a kb::iface::SpiDevice and kb::iface GPIO pins backed by the driver's
// own chip model (components/drivers/driver-tle92466ed/tests/mock_tle92466ed_bus.hpp).
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include <cstdint>
#include <span>

#include "binding_support.hpp"
#include "kb/handlers/bindings/tle92466ed.hpp"
#include "kb_test.hpp"
#include "mock_tle92466ed_bus.hpp"

namespace {

using kb::handlers::HealthState;
using kb::iface::Level;
using tle92466ed::Channel;
namespace icc = kb::handlers::tle92466ed;

/// kb::iface::SpiDevice over the driver's chip model and its fault injection. The model's clock
/// follows the RTOS clock, so the driver's waits and the SPI watchdog run in the model too.
class ModelDev {
public:
  tle92466ed_test::MockBusState state{};

  [[nodiscard]] kb::Status EnsureReady() noexcept {
    return state.bus_up_ok ? kb::Status::Ok : kb::Status::NotReady;
  }
  [[nodiscard]] kb::Status Transfer(std::span<const uint8_t> tx, std::span<uint8_t> rx) noexcept {
    Sync();
    const std::size_t len = (tx.size() > rx.size()) ? tx.size() : rx.size();
    return state.DoTransfer(tx.empty() ? nullptr : tx.data(), rx.empty() ? nullptr : rx.data(), len)
               ? kb::Status::Ok
               : kb::Status::BusError;
  }
  void Sync() noexcept {
    const uint64_t now = kb::rtos::Now().Us();
    state.chip.Elapse(static_cast<uint32_t>(now - last_us_));
    last_us_ = now;
  }

private:
  uint64_t last_us_{kb::rtos::Now().Us()}; // the model's clock starts with the object
};
static_assert(kb::iface::SpiDevice<ModelDev>);

/// RESN (electrically active low) or EN (active high) of the model.
class ModelOutput {
public:
  ModelOutput(ModelDev& dev, bool resn) noexcept : dev_(dev), resn_(resn) {}
  [[nodiscard]] kb::Status Set(Level level) noexcept {
    dev_.Sync();
    const bool high = level == Level::High;
    if (resn_) {
      dev_.state.chip.SetResn(high);
    } else {
      dev_.state.chip.SetEn(high);
    }
    level_ = level;
    return kb::Status::Ok;
  }
  [[nodiscard]] kb::Status Toggle() noexcept {
    return Set((level_ == Level::High) ? Level::Low : Level::High);
  }

private:
  ModelDev& dev_;
  bool resn_;
  Level level_{Level::Low};
};

/// FAULTN of the model: open drain, low while a fault is reported.
class FaultInput {
public:
  explicit FaultInput(ModelDev& dev) noexcept : dev_(dev) {}
  [[nodiscard]] kb::Status Get(Level& out) noexcept {
    out = dev_.state.chip.FaultPinLow() ? Level::Low : Level::High;
    return kb::Status::Ok;
  }

private:
  ModelDev& dev_;
};
static_assert(kb::iface::GpioOutput<ModelOutput> && kb::iface::GpioInput<FaultInput>);

using Handler = icc::Handler<ModelDev>;
using GuardedBus = kb::handlers::BusDomain<ModelDev, kb::handlers::ThreadModel::Guarded>;
using OwnerBus = kb::handlers::BusDomain<ModelDev, kb::handlers::ThreadModel::Owner>;
using GuardedIo = kb::handlers::Guarded<Handler, GuardedBus>;
using OwnedIo = kb::handlers::OwnerAccess<Handler, OwnerBus>;

constexpr kb::Milliseconds kWait{500U};

/// The board: the model, RESN, EN and FAULTN (DRV0 and DRV1 not wired).
struct Board {
  ModelDev dev{};
  ModelOutput resn{dev, true};
  ModelOutput en{dev, false};
  FaultInput fault{dev};

  icc::Config Wired() {
    icc::Config c{};
    c.chip.channels[0] = tle92466ed::CurrentControlChannel();
    c.chip.channels[1] = tle92466ed::CurrentControlChannel();
    c.outputs[icc::kPinReset].pin.emplace(resn);
    c.outputs[icc::kPinReset].active_low = true;
    c.outputs[icc::kPinEnable].pin.emplace(en);
    c.inputs[icc::kPinFault].pin.emplace(fault);
    c.inputs[icc::kPinFault].active_low = true;
    c.handler.health.fault_after = 2U;
    c.handler.recovery.backoff = kb::Milliseconds{1U};
    return c;
  }
};

/// A guarded handler on its own bus, created.
struct GuardedRig {
  kb_it::Timing t{};
  Board board{};
  GuardedBus domain{board.dev};
  GuardedIo io;
  GuardedRig() : io(domain, board.dev, t.timing, board.Wired()) {
    static_cast<void>(domain.Create());
    static_cast<void>(io.Create());
  }
  tle92466ed_test::MockBusState& Bus() {
    return board.dev.state;
  }
};

KB_TEST("tle92466ed: guarded start runs the one sync stage and ends in Config Mode with every "
        "channel off") {
  GuardedRig r{};
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Unknown);
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  const kb::handlers::StartupReport rep = r.io.Startup().Value();
  KB_CHECK(rep.complete);
  KB_CHECK_EQ(rep.stages, 1U);
  KB_CHECK_EQ(rep.kinds, kb::handlers::stage::kIdentify | kb::handlers::stage::kConfigure);
  KB_CHECK(!r.Bus().chip.InReset());
  KB_CHECK(!r.Bus().chip.MissionMode());
  KB_CHECK(r.io.EnsureStarted(kWait).IsOk()); // idempotent
  KB_CHECK_EQ(r.io.Health().Value().starts, 1U);
}

KB_TEST("tle92466ed: calls before a start send nothing and return NotReady") {
  GuardedRig r{};
  const int before = r.Bus().transfers;
  KB_CHECK(icc::EnterMissionMode(r.io, kWait).Error() == kb::Status::NotReady);
  KB_CHECK(r.io.Sample(kWait).Error() == kb::Status::NotReady);
  KB_CHECK_EQ(r.Bus().transfers, before);
}

KB_TEST("tle92466ed: MISO stuck low fails the start with NoDevice and the driver's code; a retry "
        "starts again") {
  GuardedRig r{};
  r.Bus().miso = tle92466ed_test::Miso::StuckLow;
  const kb::Result<void> first = r.io.EnsureStarted(kWait);
  KB_CHECK(first.Error() == kb::Status::NoDevice);
  KB_CHECK_EQ(first.Detail(), kb::handlers::DetailOf(tle92466ed::Error::NoDevice));
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Faulted);
  KB_CHECK_EQ(r.io.Health().Value().start_failures, 1U);
  r.Bus().miso = tle92466ed_test::Miso::Chip;
  KB_CHECK(r.io.EnsureStarted(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
}

KB_TEST("tle92466ed: the app API sets currents, enters Mission Mode, switches channels and reads "
        "diagnosis") {
  GuardedRig r{};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  const kb::Result<uint32_t> set = icc::SetCurrent(r.io, Channel::Ch0, 500'000U, kWait);
  KB_REQUIRE(set.IsOk());
  KB_CHECK((set.Value() > 499'000U) && (set.Value() < 501'000U));
  KB_CHECK(icc::SetCurrent(r.io, Channel::Ch1, 300'000U, kWait).IsOk());
  KB_CHECK(icc::EnterMissionMode(r.io, kWait).IsOk());
  KB_CHECK(r.Bus().chip.MissionMode());
  KB_CHECK(icc::SetEnabledChannels(r.io, 0x03U, kWait, tle92466ed::Verify::ReadBack).IsOk());
  const auto snap = r.io.Sample(kWait);
  KB_REQUIRE(snap.IsOk());
  KB_CHECK(snap.Value().value.mission);
  KB_CHECK_EQ(snap.Value().value.enabled, 0x03U);
  KB_CHECK(snap.Value().value.status.init_done);
  KB_CHECK(icc::DisableChannel(r.io, Channel::Ch1, kWait).IsOk());
  KB_CHECK_EQ(r.io.Sample(kWait).Value().value.enabled, 0x01U);
  KB_CHECK(icc::ReadFaults(r.io, kWait).IsOk());
  KB_CHECK(icc::ClearFaults(r.io, kWait).IsOk());
  icc::Command supply{};
  supply.op = icc::Op::ReadSupply;
  const auto vbat = r.io.Request(supply, kWait);
  KB_REQUIRE(vbat.IsOk());
  KB_CHECK((vbat.Value().value > 13'500U) && (vbat.Value().value < 14'500U));
  icc::Command bad{};
  bad.op = icc::Op::EnableChannel;
  bad.channel = 6U;
  KB_CHECK(r.io.Request(bad, kWait).Error() == kb::Status::InvalidArgument);
  KB_CHECK(icc::EnterConfigMode(r.io, kWait).IsOk());
  KB_CHECK(!r.Bus().chip.MissionMode());
}

KB_TEST("tle92466ed: bus failures degrade, then fault without touching the outputs; Recover() "
        "switches them off") {
  GuardedRig r{};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  KB_REQUIRE(icc::SetCurrent(r.io, Channel::Ch0, 400'000U, kWait).IsOk());
  KB_REQUIRE(icc::EnterMissionMode(r.io, kWait).IsOk());
  KB_REQUIRE(icc::SetEnabledChannels(r.io, 0x01U, kWait).IsOk());
  r.Bus().fail_transfer = r.Bus().transfers; // the next frame fails
  const auto first = r.io.Sample(kWait);
  KB_CHECK(first.Error() == kb::Status::BusError);
  KB_CHECK_EQ(first.Detail(), kb::handlers::DetailOf(tle92466ed::Error::Bus));
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Degraded);
  r.Bus().fail_transfer = r.Bus().transfers;
  KB_CHECK(!r.io.Sample(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Faulted);
  KB_CHECK(r.Bus().chip.MissionMode()); // the handler only reports: the channel stays as it was
  const int before = r.Bus().transfers;
  KB_CHECK(icc::DisableChannel(r.io, Channel::Ch0, kWait).Error() == kb::Status::NotReady);
  KB_CHECK_EQ(r.Bus().transfers, before); // a Faulted handler sends nothing
  KB_CHECK(r.io.RequestRecovery(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  KB_CHECK(!r.Bus().chip.MissionMode()); // Deinitialize() and the plan: Config Mode, channels off
  KB_CHECK_EQ(r.io.Health().Value().recoveries, 1U);
}

KB_TEST(
    "tle92466ed: a power-on reset degrades (an empty reply), then UnexpectedReset faults at once") {
  GuardedRig r{};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  r.Bus().chip.PowerOn();
  kb::Result<kb::handlers::Snapshot<icc::Snapshot>> s = r.io.Sample(kWait);
  if (s.Error() == kb::Status::NoDevice) {
    KB_CHECK(kb_it::StateOf(r.io) == HealthState::Degraded);
    s = r.io.Sample(kWait);
  }
  KB_CHECK(s.Error() == kb::Status::NotReady);
  KB_CHECK_EQ(s.Detail(), kb::handlers::DetailOf(tle92466ed::Error::UnexpectedReset));
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Faulted);
  KB_CHECK(r.io.RequestRecovery(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
}

KB_TEST("tle92466ed: a critical fault is fatal at once; recovery leaves it through RESN") {
  GuardedRig r{};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  r.Bus().chip.EnterCritical(0xE0U);
  const auto s = r.io.Sample(kWait);
  KB_CHECK(s.Error() == kb::Status::Fault);
  KB_CHECK_EQ(s.Detail(), kb::handlers::DetailOf(tle92466ed::Error::CriticalFault));
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Faulted);
  KB_CHECK(!r.Bus().chip.En()); // the driver drove EN low itself
  KB_CHECK(r.io.RequestRecovery(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  KB_CHECK(r.io.Sample(kWait).IsOk());
}

KB_TEST("tle92466ed: owner model: the owner task feeds the SPI watchdog while it samples, and "
        "serves requests") {
  kb_it::Timing t{};
  Board board{};
  OwnerBus domain{board.dev};
  icc::Config cfg = board.Wired();
  cfg.chip.spi_watchdog = true;
  cfg.chip.watchdog_reload = 200U; // about 117 ms
  cfg.handler.sample_period = kb::Milliseconds{2U};
  OwnedIo io{domain, board.dev, t.timing, cfg};
  KB_REQUIRE(io.Create().IsOk());
  const kb_it::OwnerLoop<OwnerBus, OwnedIo> owner{domain, io};
  KB_REQUIRE(io.EnsureStarted(kWait).IsOk());
  KB_CHECK(icc::SetCurrent(io, Channel::Ch0, 200'000U, kWait).IsOk());
  KB_CHECK(icc::EnterMissionMode(io, kWait).IsOk());
  const uint32_t before = io.Latest().ValueOr(kb::handlers::Snapshot<icc::Snapshot>{}).sequence;
  kb::rtos::SleepFor(kb::Milliseconds{300U}); // well past the watchdog time
  const auto latest = io.Latest();
  KB_REQUIRE(latest.IsOk());
  KB_CHECK(latest.Value().sequence > (before + 2U)); // the owner kept sampling
  KB_CHECK(!latest.Value().value.status.spi_watchdog_error);
  KB_CHECK(latest.Value().value.mission);
  KB_CHECK(kb_it::StateOf(io) == HealthState::Ready);
  KB_CHECK(io.Liveness().Beats() > 0U);
  KB_CHECK(io.Stop(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(io) == HealthState::Unknown);
  KB_CHECK(!board.dev.state.chip.MissionMode());
}

} // namespace
