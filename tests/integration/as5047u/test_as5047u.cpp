// as5047u through kb::handlers::as5047u: start-up, the app API, error mapping to the health record,
// recovery, and both thread models, over a kb::iface::SpiDevice backed by the driver's own
// frame-level chip model (components/drivers/driver-as5047u/tests/mock_as5047u_bus.hpp).
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include <cstdint>
#include <span>

#include "binding_support.hpp"
#include "kb/handlers/bindings/as5047u.hpp"
#include "kb_test.hpp"
#include "mock_as5047u_bus.hpp"

namespace {

using kb::handlers::HealthState;
namespace enc = kb::handlers::as5047u;

/// kb::iface::SpiDevice over the driver's chip model. The model's clock follows the RTOS clock, so
/// the driver's waits (DIA polling after a reset) pass in the model as they pass on the host.
class ModelDev {
public:
  as5047u_test::MockBusState state{};

  [[nodiscard]] kb::Status EnsureReady() noexcept {
    return state.bus_up_ok ? kb::Status::Ok : kb::Status::NotReady;
  }
  [[nodiscard]] kb::Status Transfer(std::span<const uint8_t> tx, std::span<uint8_t> rx) noexcept {
    sync();
    const std::size_t len = (tx.size() > rx.size()) ? tx.size() : rx.size();
    return state.DoTransfer(tx.empty() ? nullptr : tx.data(), rx.empty() ? nullptr : rx.data(), len)
               ? kb::Status::Ok
               : kb::Status::BusError;
  }

private:
  void sync() noexcept {
    const uint64_t now = kb::rtos::Now().Us();
    state.chip.Elapse(static_cast<uint32_t>(now - last_us_));
    last_us_ = now;
  }
  uint64_t last_us_{kb::rtos::Now().Us()}; // the model's clock starts with the object
};
static_assert(kb::iface::SpiDevice<ModelDev>);

using Handler = enc::Handler<ModelDev>;
using GuardedBus = kb::handlers::BusDomain<ModelDev, kb::handlers::ThreadModel::Guarded>;
using OwnerBus = kb::handlers::BusDomain<ModelDev, kb::handlers::ThreadModel::Owner>;
using GuardedIo = kb::handlers::Guarded<Handler, GuardedBus>;
using OwnedIo = kb::handlers::OwnerAccess<Handler, OwnerBus>;

constexpr kb::Milliseconds kWait{200U};

enc::Config Reversed() {
  enc::Config c{};
  c.chip.direction = as5047u::Direction::CounterClockwise;
  c.chip.zero_position = 4000U; // differs from the blank OTP, so a reset shows
  c.sample_field = true;
  c.handler.health.fault_after = 2U;
  c.handler.recovery.backoff = kb::Milliseconds{1U};
  return c;
}

/// A guarded handler on its own bus, created.
struct GuardedRig {
  kb_it::Timing t{};
  ModelDev dev{};
  GuardedBus domain{dev};
  GuardedIo io;
  explicit GuardedRig(const enc::Config& cfg) : io(domain, dev, t.timing, cfg) {
    static_cast<void>(domain.Create());
    static_cast<void>(io.Create());
  }
};

// The angle the model reports for a raw magnet angle under Reversed(): direction, then the zero.
uint16_t Expected(uint16_t raw) {
  return static_cast<uint16_t>(((16384U - raw) - 4000U) & 0x3FFFU);
}

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

KB_TEST("as5047u: guarded start runs the one sync stage, writes the settings and is Ready") {
  GuardedRig r{Reversed()};
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Unknown);
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  const kb::handlers::StartupReport rep = r.io.Startup().Value();
  KB_CHECK(rep.complete);
  KB_CHECK_EQ(rep.stages, 1U);
  KB_CHECK_EQ(rep.kinds, kb::handlers::stage::kIdentify | kb::handlers::stage::kConfigure);
  KB_CHECK((r.dev.state.chip.nv[4] & as5047u_test::chip::kS2Dir) !=
           0U);                               // direction reached the chip
  KB_CHECK(r.io.EnsureStarted(kWait).IsOk()); // idempotent
  KB_CHECK_EQ(r.io.Health().Value().starts, 1U);
}

KB_TEST("as5047u: calls before a start send nothing and return NotReady") {
  GuardedRig r{Reversed()};
  const int before = r.dev.state.transfers;
  KB_CHECK(enc::ReadAngle(r.io, kWait).Error() == kb::Status::NotReady);
  KB_CHECK(r.io.Sample(kWait).Error() == kb::Status::NotReady);
  KB_CHECK_EQ(r.dev.state.transfers, before);
}

KB_TEST("as5047u: a floating MISO fails the start with NoDevice and the driver's code; a retry "
        "starts again") {
  GuardedRig r{Reversed()};
  r.dev.state.chip.line = as5047u_test::LineMode::FloatHigh;
  const kb::Result<void> first = r.io.EnsureStarted(kWait);
  KB_CHECK(first.Error() == kb::Status::NoDevice);
  KB_CHECK_EQ(first.Detail(), kb::handlers::DetailOf(as5047u::Error::NoDevice));
  const kb::handlers::HealthRecord h = r.io.Health().Value();
  KB_CHECK(h.state == HealthState::Faulted);
  KB_CHECK_EQ(h.start_failures, 1U);
  const kb::handlers::StartupReport rep = r.io.Startup().Value();
  KB_CHECK(!rep.complete);
  KB_CHECK(rep.status == kb::Status::NoDevice);
  r.dev.state.chip.line = as5047u_test::LineMode::Normal;
  KB_CHECK(r.io.EnsureStarted(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
}

KB_TEST("as5047u: the app API reads angle and velocity, zeroes, turns the direction and checks for "
        "a reset") {
  GuardedRig r{Reversed()};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  r.dev.state.chip.raw_angle = 1000U;
  r.dev.state.chip.velocity = 0x3FF0U; // -16 counts
  const kb::Result<uint16_t> angle = enc::ReadAngle(r.io, kWait);
  KB_REQUIRE(angle.IsOk());
  KB_CHECK_EQ(angle.Value(), Expected(1000U));
  const kb::Result<int16_t> vel = enc::ReadVelocity(r.io, kWait);
  KB_REQUIRE(vel.IsOk());
  KB_CHECK_EQ(vel.Value(), -16);
  KB_CHECK(enc::SetZeroHere(r.io, kWait).IsOk());
  KB_CHECK_EQ(enc::ReadAngle(r.io, kWait).Value(), 0U);
  KB_CHECK(enc::SetDirection(r.io, as5047u::Direction::Clockwise, kWait).IsOk());
  KB_CHECK((r.dev.state.chip.nv[4] & as5047u_test::chip::kS2Dir) == 0U);
  KB_CHECK(enc::CheckForReset(r.io, kWait).IsOk());
  KB_CHECK(enc::ReadErrorFlags(r.io, kWait).IsOk());
  r.dev.state.chip.agc = 77U;
  const auto snap = r.io.Sample(kWait);
  KB_REQUIRE(snap.IsOk());
  KB_CHECK_EQ(snap.Value().value.agc, 77U);
  KB_CHECK_EQ(snap.Value().value.magnitude, 4000U);
  KB_CHECK(r.io.Latest().IsOk());
  enc::Command c{};
  c.op = enc::Op::ReadDiagnostics;
  const auto dia = r.io.Request(c, kWait);
  KB_REQUIRE(dia.IsOk());
  KB_CHECK((dia.Value().value & as5047u_test::chip::kDiaLoopsFinished) != 0U);
}

KB_TEST("as5047u: a magnet warning and a bus failure degrade, the threshold faults, Recover() "
        "restores Ready") {
  GuardedRig r{Reversed()};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  r.dev.state.chip.errfl = static_cast<uint16_t>(as5047u_test::chip::kAgcWarning);
  const kb::Result<uint16_t> warn = enc::ReadAngle(r.io, kWait);
  KB_CHECK(warn.Error() == kb::Status::OutOfRange);
  KB_CHECK_EQ(warn.Detail(), kb::handlers::DetailOf(as5047u::Error::MagnetWarning));
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Degraded);
  KB_CHECK(enc::ReadAngle(r.io, kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  r.dev.state.fail_transfer = r.dev.state.transfers; // the next frame fails on the bus
  const kb::Result<uint16_t> bad = enc::ReadAngle(r.io, kWait);
  KB_CHECK(bad.Error() == kb::Status::BusError);
  KB_CHECK_EQ(bad.Detail(), kb::handlers::DetailOf(as5047u::Error::Bus));
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Degraded);
  r.dev.state.chip.line = as5047u_test::LineMode::FloatHigh;
  KB_CHECK(!enc::ReadAngle(r.io, kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Faulted);
  const int before = r.dev.state.transfers;
  KB_CHECK(enc::ReadAngle(r.io, kWait).Error() == kb::Status::NotReady);
  KB_CHECK_EQ(r.dev.state.transfers, before); // reads never restart a faulted chip
  r.dev.state.chip.line = as5047u_test::LineMode::Normal;
  KB_CHECK(r.io.RequestRecovery(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  KB_CHECK_EQ(r.io.Health().Value().recoveries, 1U);
}

KB_TEST("as5047u: a power-on reset of the chip is fatal at once, and recovery writes the settings "
        "again") {
  GuardedRig r{Reversed()};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  r.dev.state.chip.PowerOnReset();
  const kb::Result<void> res = enc::CheckForReset(r.io, kWait);
  KB_CHECK(res.Error() == kb::Status::NotReady);
  KB_CHECK_EQ(res.Detail(), kb::handlers::DetailOf(as5047u::Error::DeviceReset));
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Faulted);
  KB_CHECK(r.io.RequestRecovery(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  r.dev.state.chip.raw_angle = 2000U;
  KB_CHECK_EQ(enc::ReadAngle(r.io, kWait).Value(), Expected(2000U)); // zero and direction again
}

KB_TEST("as5047u: owner model: the owner task starts, samples at its period and serves requests") {
  kb_it::Timing t{};
  ModelDev dev{};
  OwnerBus domain{dev};
  enc::Config cfg = Reversed();
  cfg.handler.sample_period = kb::Milliseconds{2U};
  OwnedIo io{domain, dev, t.timing, cfg};
  KB_REQUIRE(io.Create().IsOk());
  dev.state.chip.raw_angle = 3000U;
  const kb_it::OwnerLoop<OwnerBus, OwnedIo> owner{domain, io};
  KB_REQUIRE(io.EnsureStarted(kWait).IsOk());
  const auto snap = io.Sample(kWait);
  KB_REQUIRE(snap.IsOk());
  KB_CHECK_EQ(snap.Value().value.angle_counts, Expected(3000U));
  KB_CHECK(SamplesPublished(io, 1U, kWait)); // the owner samples on its own
  KB_CHECK(enc::SetZeroHere(io, kWait).IsOk());
  KB_CHECK_EQ(enc::ReadAngle(io, kWait).Value(), 0U);
  KB_CHECK(io.Liveness().Beats() > 0U);
  KB_CHECK(io.Stop(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(io) == HealthState::Unknown);
}

} // namespace
