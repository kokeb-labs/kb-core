// alicat_basis2 through kb::handlers::alicat_basis2: start-up, the app API, error mapping to the
// health record, recovery with the driver's own Recover(), and both thread models, over a
// kb::iface::UartPort backed by the driver's own instrument and wire model
// (components/drivers/driver-alicat-basis2/tests/mock_basis2.hpp). The driver's millisecond clock
// is the model's virtual clock, so the reply waits do not depend on the host's load.
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include <cstdint>
#include <span>

#include "binding_support.hpp"
#include "kb/handlers/bindings/alicat_basis2.hpp"
#include "kb_test.hpp"
#include "mock_basis2.hpp"

namespace {

using kb::handlers::HealthState;
namespace ab = kb::handlers::alicat_basis2;

/// kb::iface::UartPort over the driver's instrument model (its wire runs on the model's virtual
/// clock).
class ModelUart {
public:
  model::Instrument inst{};
  bool ready_ok{true};

  [[nodiscard]] kb::Status EnsureReady() noexcept {
    return ready_ok ? kb::Status::Ok : kb::Status::NotReady;
  }
  [[nodiscard]] kb::Status Write(std::span<const uint8_t> tx) noexcept {
    return inst.WriteBytes(tx.data(), tx.size()) ? kb::Status::Ok : kb::Status::BusError;
  }
  [[nodiscard]] kb::Result<std::size_t> Read(std::span<uint8_t> rx,
                                             kb::Milliseconds timeout) noexcept {
    static_cast<void>(timeout); // the driver only polls (0)
    return kb::Result<std::size_t>::Success(inst.ReadBytes(rx.data(), rx.size()));
  }
  [[nodiscard]] kb::Status FlushInput() noexcept {
    if (inst.fail_flush) {
      return kb::Status::BusError;
    }
    inst.Flush();
    return kb::Status::Ok;
  }
  [[nodiscard]] kb::Status SetBaud(uint32_t baud) noexcept {
    inst.port_baud = baud;
    ++inst.baud_sets;
    return kb::Status::Ok;
  }
};

/// The driver's clock (through HookTiming): the model's virtual clock, one millisecond per reading,
/// so every reply wait and the instrument's timing agree however loaded the host is.
struct ModelClock {
  model::Instrument* inst;
  [[nodiscard]] kb::Instant Now() const noexcept {
    inst->now_us += 1000U;
    return kb::Instant{inst->now_us};
  }
};

/// Never called: the driver sleeps with DelayMs.
struct NoSpin {
  void WaitUs(kb::Microseconds us) noexcept {
    static_cast<void>(us);
  }
};

/// A model line and the timing of the hooks over it.
struct Line {
  ModelUart uart{};
  ModelClock clock{&uart.inst};
  NoSpin spin{};
  kb::handlers::HookTiming timing{kb::iface::MonotonicClockRef{clock},
                                  kb::iface::BusyWaitRef{spin}};
};
static_assert(kb::iface::UartPort<ModelUart>);
static_assert(kb::iface::UartBaudControl<ModelUart>);

using Handler = ab::Handler<ModelUart>;
using GuardedBus = kb::handlers::BusDomain<ModelUart, kb::handlers::ThreadModel::Guarded>;
using OwnerBus = kb::handlers::BusDomain<ModelUart, kb::handlers::ThreadModel::Owner>;
using GuardedIo = kb::handlers::Guarded<Handler, GuardedBus>;
using OwnedIo = kb::handlers::OwnerAccess<Handler, OwnerBus>;

constexpr kb::Milliseconds kWait{5000U};

ab::Config Controller() {
  ab::Config c{};
  c.handler.health.fault_after = 2U;
  c.handler.recovery.backoff = kb::Milliseconds{1U};
  c.chip.read_retries = 0U;
  return c;
}

/// A guarded handler on its own line, created.
struct GuardedRig {
  Line line{};
  ModelUart& uart{line.uart};
  GuardedBus domain{uart};
  GuardedIo io;
  explicit GuardedRig(const ab::Config& cfg) : io(domain, uart, line.timing, cfg) {
    static_cast<void>(domain.Create());
    static_cast<void>(io.Create());
  }
};

KB_TEST("alicat_basis2: guarded start runs Link then Identify and the handler is Ready") {
  GuardedRig r{Controller()};
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Unknown);
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  const kb::handlers::StartupReport report = r.io.Startup().Value();
  KB_CHECK(report.complete);
  KB_CHECK_EQ(report.stages, 2U);
  KB_CHECK_EQ(report.stage, 1U);
  KB_CHECK_EQ(report.kinds, kb::handlers::stage::kIdentify);
  KB_CHECK(!r.uart.inst.writes.empty()); // Begin() talked to the instrument
  const std::size_t sent = r.uart.inst.writes.size();
  KB_CHECK(r.io.EnsureStarted(kWait).IsOk()); // idempotent, no traffic
  KB_CHECK_EQ(r.uart.inst.writes.size(), sent);
  KB_CHECK_EQ(r.io.Health().Value().starts, 1U);
}

KB_TEST("alicat_basis2: calls before a start send nothing and return NotReady") {
  GuardedRig r{Controller()};
  KB_CHECK(ab::ReadFrame(r.io, kWait).Error() == kb::Status::NotReady);
  KB_CHECK(ab::SetSetpoint(r.io, 10.0, kWait).Error() == kb::Status::NotReady);
  KB_CHECK(r.io.Sample(kWait).Error() == kb::Status::NotReady);
  KB_CHECK(r.uart.inst.writes.empty());
}

KB_TEST("alicat_basis2: an instrument on another ID fails Identify with Timeout; a retry starts "
        "from Link") {
  GuardedRig r{Controller()};
  r.uart.inst.unit_id = 'B';
  const kb::Result<void> first = r.io.EnsureStarted(kWait);
  KB_CHECK(first.Error() == kb::Status::Timeout);
  KB_CHECK_EQ(first.Detail(), kb::handlers::DetailOf(alicat_basis2::Error::Timeout));
  const kb::handlers::StartupReport rep = r.io.Startup().Value();
  KB_CHECK(!rep.complete);
  KB_CHECK_EQ(rep.stage, 1U);
  KB_CHECK_EQ(rep.kinds, kb::handlers::stage::kIdentify);
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Faulted);
  KB_CHECK_EQ(r.io.Health().Value().start_failures, 1U);
  r.uart.inst.unit_id = 'A';
  KB_CHECK(r.io.EnsureStarted(kWait).IsOk());
  KB_CHECK_EQ(r.io.Startup().Value().runs, 2U);
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
}

KB_TEST("alicat_basis2: a port that does not come up fails Link with BusError") {
  GuardedRig r{Controller()};
  r.uart.ready_ok = false;
  const kb::Result<void> first = r.io.EnsureStarted(kWait);
  KB_CHECK(first.Error() == kb::Status::BusError);
  KB_CHECK_EQ(first.Detail(), kb::handlers::DetailOf(alicat_basis2::Error::Bus));
  KB_CHECK_EQ(r.io.Startup().Value().stage, 0U);
  KB_CHECK(r.uart.inst.writes.empty());
}

KB_TEST(
    "alicat_basis2: setpoint, valve, gas, totalizer and frame reads reach the instrument model") {
  GuardedRig r{Controller()};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  KB_CHECK(ab::SetSetpoint(r.io, 150.0, kWait).IsOk());
  KB_CHECK_EQ(r.uart.inst.setpoint_milli, 150000);
  const kb::Result<alicat_basis2::Decimal> sp = ab::ReadSetpoint(r.io, kWait);
  KB_REQUIRE(sp.IsOk());
  KB_CHECK_EQ(sp.Value().ToMilli(), 150000);
  KB_CHECK(ab::HoldValve(r.io, 40.0, kWait).IsOk());
  KB_CHECK(r.uart.inst.hld);
  KB_CHECK(ab::ResumeControl(r.io, kWait).IsOk());
  KB_CHECK(!r.uart.inst.hld);
  KB_CHECK(ab::CloseValve(r.io, kWait).IsOk());
  KB_CHECK(ab::ResumeControl(r.io, kWait).IsOk());
  KB_CHECK(ab::SetGas(r.io, alicat_basis2::GasId{3U}, kWait).IsOk());
  KB_CHECK_EQ(r.uart.inst.gas, 3);
  ab::Command reset{};
  reset.op = ab::Op::ResetTotal;
  KB_CHECK(r.io.Request(reset, kWait).IsOk());
  ab::Command source{};
  source.op = ab::Op::SetSetpointSource;
  source.source = alicat_basis2::SetpointSource::UnsavedDigital;
  KB_CHECK(r.io.Request(source, kWait).IsOk());
  KB_CHECK(ab::Tare(r.io, 50U, kWait).IsOk());
  r.uart.inst.flow_milli = 42000;
  const kb::Result<alicat_basis2::Frame> frame = ab::ReadFrame(r.io, kWait);
  KB_REQUIRE(frame.IsOk());
  KB_CHECK_EQ(frame.Value().flow.ToMilli(), 42000);
  const auto snap = r.io.Sample(kWait);
  KB_REQUIRE(snap.IsOk());
  KB_CHECK_EQ(snap.Value().value.frame.flow.ToMilli(), 42000);
  KB_CHECK(r.io.Latest().IsOk());
}

KB_TEST("alicat_basis2: the error map and the fatal set") {
  static_assert(ab::ToStatus(alicat_basis2::Error::Ok) == kb::Status::Ok);
  static_assert(ab::ToStatus(alicat_basis2::Error::Timeout) == kb::Status::Timeout);
  static_assert(ab::ToStatus(alicat_basis2::Error::Rejected) == kb::Status::Fault);
  static_assert(ab::ToStatus(alicat_basis2::Error::StateUnknown) == kb::Status::NotReady);
  static_assert(ab::ToStatus(alicat_basis2::Error::RateLimited) == kb::Status::Busy);
  static_assert(ab::ToStatus(alicat_basis2::Error::Crc) == kb::Status::CrcMismatch);
  KB_CHECK(ab::IsFatalError(alicat_basis2::Error::StateUnknown));
  KB_CHECK(ab::IsFatalError(alicat_basis2::Error::NotInitialized));
  KB_CHECK(!ab::IsFatalError(alicat_basis2::Error::Timeout));
}

KB_TEST("alicat_basis2: a rejected reply and a lost reply degrade, the threshold faults, Recover() "
        "restores Ready") {
  GuardedRig r{Controller()};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  r.uart.inst.faults.count = 1U;
  r.uart.inst.faults.error_reply = true;
  const kb::Result<alicat_basis2::Frame> rejected = ab::ReadFrame(r.io, kWait);
  KB_CHECK(rejected.Error() == kb::Status::Fault);
  KB_CHECK_EQ(rejected.Detail(), kb::handlers::DetailOf(alicat_basis2::Error::Rejected));
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Degraded);
  KB_CHECK(ab::ReadFrame(r.io, kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  r.uart.inst.faults = model::Faults{};
  r.uart.inst.faults.count = 2U;
  r.uart.inst.faults.drop = true;
  const kb::Result<alicat_basis2::Frame> lost = ab::ReadFrame(r.io, kWait);
  KB_CHECK(lost.Error() == kb::Status::Timeout);
  KB_CHECK_EQ(lost.Detail(), kb::handlers::DetailOf(alicat_basis2::Error::Timeout));
  KB_CHECK(!ab::ReadFrame(r.io, kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Faulted);
  const std::size_t sent = r.uart.inst.writes.size();
  KB_CHECK(ab::ReadFrame(r.io, kWait).Error() == kb::Status::NotReady);
  KB_CHECK_EQ(r.uart.inst.writes.size(), sent); // reads never restart a faulted instrument
  KB_CHECK(r.io.RequestRecovery(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  KB_CHECK_EQ(r.io.Health().Value().recoveries, 1U);
}

KB_TEST("alicat_basis2: recovery adopts the unit ID the driver's Recover() found") {
  GuardedRig r{Controller()};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  r.uart.inst.unit_id = 'C'; // the instrument now answers to another ID
  KB_CHECK(ab::ReadFrame(r.io, kWait).Error() == kb::Status::Timeout);
  KB_CHECK(ab::ReadFrame(r.io, kWait).Error() == kb::Status::Timeout);
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Faulted);
  KB_CHECK(r.io.RequestRecovery(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  KB_CHECK(ab::ReadFrame(r.io, kWait).IsOk());
  KB_CHECK(model::SameChar(r.uart.inst.last_command.front(), 'C'));
}

KB_TEST("alicat_basis2: owner model: the owner task starts, samples and serves requests") {
  Line line{};
  ModelUart& uart = line.uart;
  OwnerBus domain{uart};
  ab::Config cfg = Controller();
  cfg.handler.sample_period = kb::Milliseconds{20U};
  OwnedIo io{domain, uart, line.timing, cfg};
  KB_REQUIRE(io.Create().IsOk());
  uart.inst.flow_milli = 74000; // before the owner task runs: the model is not thread-safe
  const kb_it::OwnerLoop<OwnerBus, OwnedIo> owner{domain, io};
  KB_REQUIRE(io.EnsureStarted(kWait).IsOk());
  KB_CHECK(ab::SetSetpoint(io, 75.0, kWait).IsOk());
  const auto snap = io.Sample(kWait);
  KB_REQUIRE(snap.IsOk());
  KB_CHECK_EQ(snap.Value().value.frame.flow.ToMilli(), 74000);
  KB_CHECK_EQ(snap.Value().value.frame.setpoint.ToMilli(), 75000);
  KB_CHECK(io.Liveness().Beats() > 0U);
  KB_CHECK(io.Stop(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(io) == HealthState::Unknown);
  KB_CHECK_EQ(uart.inst.setpoint_milli, 75000); // the owner is idle once stopped
}

} // namespace
