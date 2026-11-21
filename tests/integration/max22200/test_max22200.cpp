// max22200 through kb::handlers::max22200: start-up, the app API, error mapping to the health
// record, what a fault does to the outputs, recovery, and both thread models, over a
// kb::iface::SpiDevice and kb::iface GPIO pins backed by the driver's own protocol model
// (components/drivers/driver-max22200/tests). SPDX-License-Identifier: MIT Copyright (c) 2026 The
// kb-core authors
#include <cstdint>
#include <span>

#include "binding_support.hpp"
#include "kb/handlers/bindings/max22200.hpp"
#include "kb_test.hpp"
#include "mock_max22200_bus.hpp"

namespace {

using kb::handlers::HealthState;
using kb::iface::Level;
namespace valves = kb::handlers::max22200;

/// kb::iface::SpiDevice over the driver's chip model and its fault injection. The model's clock
/// follows the RTOS clock, so the driver's waits (tEN, tWU) pass in the model too.
class ModelDev {
public:
  max22200_test::MockBusState state{};

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

/// ENABLE or CMD of the model (both active high).
class ModelOutput {
public:
  ModelOutput(ModelDev& dev, bool enable) noexcept : dev_(dev), enable_(enable) {}
  [[nodiscard]] kb::Status Set(Level level) noexcept {
    dev_.Sync();
    const bool high = level == Level::High;
    if (enable_) {
      dev_.state.chip.SetEnable(high);
    } else {
      dev_.state.chip.SetCmd(high);
    }
    level_ = level;
    ++writes;
    return kb::Status::Ok;
  }
  [[nodiscard]] kb::Status Toggle() noexcept {
    return Set((level_ == Level::High) ? Level::Low : Level::High);
  }
  int writes{0};

private:
  ModelDev& dev_;
  bool enable_;
  Level level_{Level::Low};
};

/// The FAULT pin of the model: open drain, low while a fault is reported.
class FaultInput {
public:
  explicit FaultInput(ModelDev& dev) noexcept : dev_(dev) {}
  [[nodiscard]] kb::Status Get(Level& out) noexcept {
    out = dev_.state.chip.FaultPin() ? Level::Low : Level::High;
    return kb::Status::Ok;
  }

private:
  ModelDev& dev_;
};
static_assert(kb::iface::GpioOutput<ModelOutput> && kb::iface::GpioInput<FaultInput>);

using Handler = valves::Handler<ModelDev>;
using GuardedBus = kb::handlers::BusDomain<ModelDev, kb::handlers::ThreadModel::Guarded>;
using OwnerBus = kb::handlers::BusDomain<ModelDev, kb::handlers::ThreadModel::Owner>;
using GuardedIo = kb::handlers::Guarded<Handler, GuardedBus>;
using OwnedIo = kb::handlers::OwnerAccess<Handler, OwnerBus>;

constexpr kb::Milliseconds kWait{500U};

/// The board: the model, ENABLE, CMD and FAULT.
struct Board {
  ModelDev dev{};
  ModelOutput enable{dev, true};
  ModelOutput cmd{dev, false};
  FaultInput fault{dev};

  valves::Config Wired(bool with_enable = true) {
    valves::Config c{};
    c.chip.loaded_channels = 0xFFU; // a valve on every channel
    if (with_enable) {
      c.outputs[valves::kPinEnable].pin.emplace(enable);
    }
    c.outputs[valves::kPinCmd].pin.emplace(cmd);
    c.inputs[valves::kPinFault].pin.emplace(fault);
    c.inputs[valves::kPinFault].active_low = true;
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
  explicit GuardedRig(bool with_enable = true)
      : io(domain, board.dev, t.timing, board.Wired(with_enable)) {
    static_cast<void>(domain.Create());
    static_cast<void>(io.Create());
  }
  max22200_test::MockBusState& Bus() {
    return board.dev.state;
  }
};

/// Waits until the owner task has published more than `count` samples, at most `timeout`.
template <typename Access>
bool SamplesPublished(const Access& io, uint32_t count, kb::Milliseconds timeout) {
  const kb::utils::Deadline end = kb::utils::Deadline::After(kb::rtos::Now(), timeout);
  bool seen = false;
  while ((!seen) && (!end.Expired(kb::rtos::Now()))) {
    const auto latest = io.Latest();
    seen = latest.IsOk() && (latest.Value().sequence > count);
    kb::rtos::SleepFor(kb::Milliseconds{1U});
  }
  return seen;
}

KB_TEST(
    "max22200: guarded start runs the one sync stage; the chip is awake with every channel off") {
  GuardedRig r{};
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Unknown);
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  const kb::handlers::StartupReport rep = r.io.Startup().Value();
  KB_CHECK(rep.complete);
  KB_CHECK_EQ(rep.stages, 1U);
  KB_CHECK_EQ(rep.kinds, kb::handlers::stage::kIdentify | kb::handlers::stage::kConfigure);
  KB_CHECK(r.Bus().chip.enable);
  KB_CHECK(r.Bus().chip.cmd); // CMD idles high
  KB_CHECK_EQ(r.Bus().chip.OnMask(), 0U);
  KB_CHECK(r.io.EnsureStarted(kWait).IsOk()); // idempotent
  KB_CHECK_EQ(r.io.Health().Value().starts, 1U);
}

KB_TEST("max22200: calls before a start send nothing and return NotReady") {
  GuardedRig r{};
  const int before = r.Bus().transfers;
  KB_CHECK(valves::SetChannelsOn(r.io, 0x01U, kWait).Error() == kb::Status::NotReady);
  KB_CHECK(r.io.Sample(kWait).Error() == kb::Status::NotReady);
  KB_CHECK_EQ(r.Bus().transfers, before);
  KB_CHECK_EQ(r.board.enable.writes, 0);
}

KB_TEST("max22200: nothing on SDO fails the start with NoDevice and the driver's code; a retry "
        "starts again") {
  GuardedRig r{};
  r.Bus().chip.miso = max22200_test::Miso::FloatHigh;
  const kb::Result<void> first = r.io.EnsureStarted(kWait);
  KB_CHECK(first.Error() == kb::Status::NoDevice);
  KB_CHECK_EQ(first.Detail(), kb::handlers::DetailOf(max22200::Error::NoDevice));
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Faulted);
  KB_CHECK_EQ(r.io.Health().Value().start_failures, 1U);
  KB_CHECK(!r.Bus().chip.enable); // the driver drives ENABLE low again on a failed start
  r.Bus().chip.miso = max22200_test::Miso::Chip;
  KB_CHECK(r.io.EnsureStarted(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
}

KB_TEST(
    "max22200: the app API switches channels, reads and clears faults and reads the FAULT pin") {
  GuardedRig r{};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  KB_CHECK(valves::SetChannelsOn(r.io, 0x05U, kWait).IsOk());
  KB_CHECK_EQ(r.Bus().chip.OnMask(), 0x05U);
  KB_CHECK(valves::SetChannel(r.io, 1U, true, kWait).IsOk());
  KB_CHECK_EQ(r.Bus().chip.OnMask(), 0x07U);
  const auto snap = r.io.Sample(kWait);
  KB_REQUIRE(snap.IsOk());
  KB_CHECK_EQ(snap.Value().value.status.on_mask, 0x07U);
  KB_CHECK_EQ(snap.Value().value.commanded, 0x07U);
  KB_CHECK(snap.Value().value.status.active);
  KB_CHECK(!valves::ReadFaultPin(r.io, kWait).Value());
  r.Bus().chip.RaiseFault(max22200_test::Fault::Olf, 2U);
  KB_CHECK(valves::ReadFaultPin(r.io, kWait).Value());
  const kb::Result<max22200::Faults> f = valves::ReadFaults(r.io, kWait);
  KB_REQUIRE(f.IsOk());
  KB_CHECK_EQ(f.Value().open_load, 0x04U);
  const kb::Result<max22200::Faults> cleared = valves::ClearFaults(r.io, kWait);
  KB_REQUIRE(cleared.IsOk());
  KB_CHECK_EQ(cleared.Value().open_load, 0x04U);
  KB_CHECK(!valves::ReadFaultPin(r.io, kWait).Value());
  KB_CHECK(valves::AllOff(r.io, kWait).IsOk());
  KB_CHECK_EQ(r.Bus().chip.OnMask(), 0U);
  valves::Command c{};
  c.op = valves::Op::ReadChannelsOn;
  const auto on = r.io.Request(c, kWait);
  KB_REQUIRE(on.IsOk());
  KB_CHECK_EQ(on.Value().value, 0U);
}

KB_TEST("max22200: bus failures degrade, then fault without touching the outputs; Recover() "
        "switches them off") {
  GuardedRig r{};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  KB_REQUIRE(valves::SetChannelsOn(r.io, 0x05U, kWait).IsOk());
  r.Bus().fail_transfer = r.Bus().transfers; // the next transfer fails
  const auto bad = r.io.Sample(kWait);
  KB_CHECK(bad.Error() == kb::Status::BusError);
  KB_CHECK_EQ(bad.Detail(), kb::handlers::DetailOf(max22200::Error::Bus));
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Degraded);
  r.Bus().fail_transfer = r.Bus().transfers;
  KB_CHECK(!r.io.Sample(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Faulted);
  KB_CHECK_EQ(r.Bus().chip.OnMask(),
              0x05U); // the handler only reports: the channels stay as they were
  const int before = r.Bus().transfers;
  KB_CHECK(valves::SetChannelsOn(r.io, 0x00U, kWait).Error() == kb::Status::NotReady);
  KB_CHECK_EQ(r.Bus().transfers, before); // a Faulted handler sends nothing
  KB_CHECK(r.io.RequestRecovery(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  KB_CHECK_EQ(r.Bus().chip.OnMask(), 0U); // Deinitialize() and the plan: every channel off
  KB_CHECK_EQ(r.io.Health().Value().recoveries, 1U);
}

KB_TEST("max22200: a power-on reset of the chip is fatal at once; recovery configures it again") {
  GuardedRig r{};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  KB_REQUIRE(valves::SetChannelsOn(r.io, 0x08U, kWait).IsOk());
  r.Bus().chip.Brownout();
  const auto res = r.io.Sample(kWait);
  KB_CHECK(res.Error() == kb::Status::NotReady);
  KB_CHECK_EQ(res.Detail(), kb::handlers::DetailOf(max22200::Error::DeviceReset));
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Faulted);
  KB_CHECK(r.io.RequestRecovery(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  KB_CHECK_EQ(r.Bus().chip.OnMask(), 0U);
  KB_CHECK(valves::SetChannelsOn(r.io, 0x08U, kWait).IsOk());
}

KB_TEST("max22200: a board with ENABLE tied high starts without the pin") {
  GuardedRig r{false};
  r.Bus().chip.SetEnable(true); // the board ties it high
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  KB_CHECK_EQ(r.board.enable.writes, 0);
  KB_CHECK(valves::SetChannelsOn(r.io, 0x01U, kWait).IsOk());
  KB_CHECK(r.io.Stop(kWait).IsOk());
  KB_CHECK_EQ(r.Bus().chip.OnMask(), 0U); // Stop() switches every channel off
}

KB_TEST("max22200: owner model: the owner task starts, supervises STATUS and serves requests") {
  kb_it::Timing t{};
  Board board{};
  OwnerBus domain{board.dev};
  valves::Config cfg = board.Wired();
  cfg.handler.sample_period = kb::Milliseconds{2U};
  OwnedIo io{domain, board.dev, t.timing, cfg};
  KB_REQUIRE(io.Create().IsOk());
  const kb_it::OwnerLoop<OwnerBus, OwnedIo> owner{domain, io};
  KB_REQUIRE(io.EnsureStarted(kWait).IsOk());
  KB_CHECK(valves::SetChannelsOn(io, 0x30U, kWait).IsOk());
  KB_CHECK(SamplesPublished(io, 1U, kWait)); // the owner reads STATUS on its own
  KB_CHECK_EQ(io.Sample(kWait).Value().value.status.on_mask, 0x30U);
  KB_CHECK(io.Liveness().Beats() > 0U);
  KB_CHECK(io.Stop(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(io) == HealthState::Unknown);
  KB_CHECK_EQ(board.dev.state.chip.OnMask(), 0U);
}

} // namespace
