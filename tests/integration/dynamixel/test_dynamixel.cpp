// dynamixel through kb::handlers::dynamixel: start-up with the per-servo identification, the app
// API, error mapping to the health record, recovery, and both thread models, over a
// kb::iface::UartPort backed by the driver's own servo bus model
// (components/drivers/driver-dynamixel/tests/mock_servo_bus.hpp), three protocol 2.0 servos on one
// bus. SPDX-License-Identifier: MIT Copyright (c) 2026 The kb-core authors
#include <algorithm>
#include <cstdint>
#include <span>

#include "binding_support.hpp"
#include "kb/handlers/bindings/dynamixel.hpp"
#include "kb_test.hpp"
#include "mock_servo_bus.hpp"

namespace {

using kb::handlers::HealthState;
namespace dx = kb::handlers::dynamixel;

/// kb::iface::UartPort over the driver's servo bus model; an empty receive waits the timeout like a
/// real port.
class ModelUart {
public:
  mock::Bus bus{};
  bool ready_ok{true};

  ModelUart() {
    bus.v2 = true;
    static_cast<void>(bus.Add(mock::Kind::Xm430, 1));
    static_cast<void>(bus.Add(mock::Kind::Xm430, 2));
    static_cast<void>(bus.Add(mock::Kind::Xm430, 3));
  }
  [[nodiscard]] kb::Status EnsureReady() noexcept {
    return ready_ok ? kb::Status::Ok : kb::Status::NotReady;
  }
  [[nodiscard]] kb::Status Write(std::span<const uint8_t> tx) noexcept {
    ++bus.writes;
    if (bus.fail_write) {
      return kb::Status::BusError;
    }
    bus.Transmit(tx.data(), tx.size());
    return kb::Status::Ok;
  }
  [[nodiscard]] kb::Result<std::size_t> Read(std::span<uint8_t> rx,
                                             kb::Milliseconds timeout) noexcept {
    ++bus.reads;
    const std::size_t n = std::min(rx.size(), bus.Queued());
    for (std::size_t i = 0U; i < n; ++i) {
      rx[i] = bus.rx[bus.rx_head++];
    }
    if ((n == 0U) && (timeout.Count() != 0U)) {
      kb::rtos::SleepFor(timeout);
    }
    return kb::Result<std::size_t>::Success(n);
  }
  [[nodiscard]] kb::Status FlushInput() noexcept {
    ++bus.flushes;
    if (bus.fail_flush) {
      return kb::Status::BusError;
    }
    bus.Clear();
    return kb::Status::Ok;
  }
  [[nodiscard]] kb::Status SetBaud(uint32_t baud) noexcept {
    ++bus.baud_calls;
    bus.baud = baud;
    return kb::Status::Ok;
  }
};
static_assert(kb::iface::UartPort<ModelUart>);

/// The servo of `id` (the first slot when none has it, so the reference is never null).
mock::Servo& ServoAt(mock::Bus& bus, uint8_t id) {
  mock::Servo* s = bus.Find(id);
  return (s != nullptr) ? *s : bus.servos[0];
}

using Handler = dx::Handler<ModelUart>;
using GuardedBus = kb::handlers::BusDomain<ModelUart, kb::handlers::ThreadModel::Guarded>;
using OwnerBus = kb::handlers::BusDomain<ModelUart, kb::handlers::ThreadModel::Owner>;
using GuardedIo = kb::handlers::Guarded<Handler, GuardedBus>;
using OwnedIo = kb::handlers::OwnerAccess<Handler, OwnerBus>;

constexpr kb::Milliseconds kWait{2000U};
constexpr dynamixel::ServoRef kS1{1U, dynamixel::Family::X430};
constexpr dynamixel::ServoRef kS2{2U, dynamixel::Family::X430};
constexpr dynamixel::ServoRef kS3{3U, dynamixel::Family::X430};

dx::Config ThreeServos() {
  dx::Config c{};
  c.chip = dynamixel::FastBusPreset();
  c.chip.retries = 0U;
  c.chip.reboot_wait_us = 1000U;
  c.servos[0] = kS1;
  c.servos[1] = kS2;
  c.servos[2] = kS3;
  c.servo_count = 3U;
  c.handler.health.fault_after = 3U;
  c.handler.recovery.backoff = kb::Milliseconds{1U};
  return c;
}

/// A guarded handler on its own bus, created.
struct GuardedRig {
  kb_it::Timing t{};
  ModelUart uart{};
  GuardedBus domain{uart};
  GuardedIo io;
  explicit GuardedRig(const dx::Config& cfg) : io(domain, uart, t.timing, cfg) {
    static_cast<void>(domain.Create());
    static_cast<void>(io.Create());
  }
};

KB_TEST("dynamixel: guarded start runs Link, then identifies each listed servo, and the handler is "
        "Ready") {
  GuardedRig r{ThreeServos()};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  const kb::handlers::StartupReport report = r.io.Startup().Value();
  KB_CHECK(report.complete);
  KB_CHECK_EQ(report.stages, 2U);
  KB_CHECK_EQ(report.stage, 1U);
  KB_CHECK_EQ(report.kinds, kb::handlers::stage::kIdentify);
  KB_CHECK_EQ(r.uart.bus.writes, 3); // one ping per servo
  KB_CHECK_EQ(r.uart.bus.baud, 57600U);
  KB_CHECK(r.io.EnsureStarted(kWait).IsOk()); // idempotent
  KB_CHECK_EQ(r.uart.bus.writes, 3);
}

KB_TEST("dynamixel: with no servo listed the Identify stage is skipped") {
  dx::Config cfg = ThreeServos();
  cfg.servo_count = 0U;
  GuardedRig r{cfg};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  KB_CHECK_EQ(r.uart.bus.writes, 0);
  KB_CHECK_EQ(r.io.Startup().Value().stage, 0U);
}

KB_TEST("dynamixel: calls before a start send nothing and return NotReady") {
  GuardedRig r{ThreeServos()};
  KB_CHECK(dx::SetTorque(r.io, kS1, true, kWait).Error() == kb::Status::NotReady);
  KB_CHECK(dx::ReadFeedback(r.io, kS1, kWait).Error() == kb::Status::NotReady);
  KB_CHECK(r.io.Sample(kWait).Error() == kb::Status::NotReady);
  KB_CHECK_EQ(r.uart.bus.writes, 0);
  KB_CHECK_EQ(r.uart.bus.ensure_calls, 0);
}

KB_TEST(
    "dynamixel: a missing servo fails Identify with Timeout and its code; a retry starts again") {
  GuardedRig r{ThreeServos()};
  ServoAt(r.uart.bus, 2U).silent = true;
  const kb::Result<void> first = r.io.EnsureStarted(kWait);
  KB_CHECK(first.Error() == kb::Status::Timeout);
  KB_CHECK_EQ(first.Detail(), kb::handlers::DetailOf(dynamixel::Error::Timeout));
  const kb::handlers::StartupReport rep = r.io.Startup().Value();
  KB_CHECK(!rep.complete);
  KB_CHECK_EQ(rep.stage, 1U);
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Faulted);
  ServoAt(r.uart.bus, 2U).silent = false;
  KB_CHECK(r.io.EnsureStarted(kWait).IsOk());
  KB_CHECK_EQ(r.io.Startup().Value().runs, 2U);
}

KB_TEST("dynamixel: a servo of another family than listed fails Identify with DeviceIdMismatch") {
  dx::Config cfg = ThreeServos();
  cfg.servos[2] = dynamixel::ServoRef{3U, dynamixel::Family::AxRxDx};
  GuardedRig r{cfg};
  const kb::Result<void> first = r.io.EnsureStarted(kWait);
  KB_CHECK(first.Error() == kb::Status::Fault);
  KB_CHECK_EQ(first.Detail(), kb::handlers::DetailOf(dynamixel::Error::DeviceIdMismatch));
  KB_CHECK_EQ(r.io.Startup().Value().stage, 1U);
}

KB_TEST("dynamixel: torque, goal position, registers, ping, reboot and the bus stop reach the "
        "servo model") {
  GuardedRig r{ThreeServos()};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  KB_CHECK(dx::SetTorque(r.io, kS1, true, kWait).IsOk());
  KB_CHECK(dx::SetTorque(r.io, kS2, true, kWait).IsOk());
  KB_CHECK(ServoAt(r.uart.bus, 1U).TorqueOn());
  KB_CHECK(dx::SetGoalPosition(r.io, kS1, 2048, kWait).IsOk());
  KB_CHECK_EQ(ServoAt(r.uart.bus, 1U).Dword(116), 2048);
  KB_CHECK(dx::WriteRegister(r.io, kS2, dynamixel::Reg::GoalPosition, -1234, kWait).IsOk());
  KB_CHECK_EQ(ServoAt(r.uart.bus, 2U).Dword(116), -1234);
  ServoAt(r.uart.bus, 3U).SetDword(132, -500);
  const kb::Result<int32_t> pos =
      dx::ReadRegister(r.io, kS3, dynamixel::Reg::PresentPosition, kWait);
  KB_REQUIRE(pos.IsOk());
  KB_CHECK_EQ(pos.Value(), -500);
  const kb::Result<dynamixel::PingInfo> ping = dx::Ping(r.io, 2U, kWait);
  KB_REQUIRE(ping.IsOk());
  KB_CHECK_EQ(ping.Value().model, 1020U);
  KB_CHECK(ping.Value().family == dynamixel::Family::X430);
  dx::Command reboot{};
  reboot.op = dx::Op::Reboot;
  reboot.servo = kS3;
  KB_CHECK(r.io.Request(reboot, kWait).IsOk());
  KB_CHECK_EQ(ServoAt(r.uart.bus, 3U).reboots, 1U);
  dx::Command resync{};
  resync.op = dx::Op::Resync;
  KB_CHECK(r.io.Request(resync, kWait).IsOk());
  KB_CHECK(dx::DisableTorqueAll(r.io, dynamixel::Family::X430, kWait).IsOk());
  KB_CHECK(!ServoAt(r.uart.bus, 1U).TorqueOn());
  KB_CHECK(!ServoAt(r.uart.bus, 2U).TorqueOn());
}

KB_TEST("dynamixel: a sample reads the feedback of every listed servo") {
  GuardedRig r{ThreeServos()};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  ServoAt(r.uart.bus, 1U).SetDword(132, 100);
  ServoAt(r.uart.bus, 2U).SetDword(132, 200);
  ServoAt(r.uart.bus, 3U).SetDword(132, 300);
  ServoAt(r.uart.bus, 2U).alert = true;
  const auto snap = r.io.Sample(kWait);
  KB_REQUIRE(snap.IsOk());
  KB_CHECK_EQ(snap.Value().value.count, 3U);
  KB_CHECK_EQ(snap.Value().value.feedback[0].position, 100);
  KB_CHECK_EQ(snap.Value().value.feedback[1].position, 200);
  KB_CHECK_EQ(snap.Value().value.feedback[2].position, 300);
  KB_CHECK_EQ(snap.Value().value.feedback[0].voltage_dv, 121U);
  KB_CHECK_EQ(snap.Value().value.alert_mask, 0x02U);
  const kb::Result<dynamixel::Feedback> one = dx::ReadFeedback(r.io, kS3, kWait);
  KB_REQUIRE(one.IsOk());
  KB_CHECK_EQ(one.Value().position, 300);
}

KB_TEST("dynamixel: a lost and a corrupt reply degrade, a stuck line faults at once, Recover() "
        "restores Ready") {
  GuardedRig r{ThreeServos()};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  r.uart.bus.knob.drop_replies = 1;
  const kb::Result<dynamixel::Feedback> lost = dx::ReadFeedback(r.io, kS1, kWait);
  KB_CHECK(lost.Error() == kb::Status::Timeout);
  KB_CHECK_EQ(lost.Detail(), kb::handlers::DetailOf(dynamixel::Error::Timeout));
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Degraded);
  r.uart.bus.knob.corrupt_crc = 1;
  const kb::Result<dynamixel::Feedback> bad = dx::ReadFeedback(r.io, kS1, kWait);
  KB_CHECK(bad.Error() == kb::Status::CrcMismatch);
  KB_CHECK_EQ(bad.Detail(), kb::handlers::DetailOf(dynamixel::Error::Crc));
  KB_CHECK_EQ(r.io.Health().Value().consecutive, 2U);
  KB_CHECK(dx::ReadFeedback(r.io, kS1, kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  r.uart.bus.knob.stuck = 1;
  const kb::Result<dynamixel::Feedback> stuck = dx::ReadFeedback(r.io, kS1, kWait);
  KB_CHECK(stuck.Error() == kb::Status::BusError);
  KB_CHECK_EQ(stuck.Detail(), kb::handlers::DetailOf(dynamixel::Error::LineFault));
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Faulted); // fatal: no threshold
  const int writes = r.uart.bus.writes;
  KB_CHECK(dx::ReadFeedback(r.io, kS1, kWait).Error() == kb::Status::NotReady);
  KB_CHECK_EQ(r.uart.bus.writes, writes);
  r.uart.bus.knob.stuck = 0;
  KB_CHECK(r.io.RequestRecovery(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  KB_CHECK_EQ(r.io.Health().Value().recoveries, 1U);
}

KB_TEST("dynamixel: a servo that stops answering faults the handler at the threshold") {
  GuardedRig r{ThreeServos()};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  ServoAt(r.uart.bus, 3U).silent = true;
  KB_CHECK(r.io.Sample(kWait).Error() == kb::Status::Timeout);
  KB_CHECK(r.io.Sample(kWait).Error() == kb::Status::Timeout);
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Degraded);
  KB_CHECK(r.io.Sample(kWait).Error() == kb::Status::Timeout);
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Faulted);
  ServoAt(r.uart.bus, 3U).silent = false;
  KB_CHECK(r.io.RequestRecovery(kWait).IsOk());
  KB_CHECK(r.io.Sample(kWait).IsOk());
}

KB_TEST("dynamixel: owner model: the owner task starts, samples and serves requests") {
  kb_it::Timing t{};
  ModelUart uart{};
  ServoAt(uart.bus, 1U).SetDword(132, 1111);
  OwnerBus domain{uart};
  dx::Config cfg = ThreeServos();
  cfg.handler.sample_period = kb::Milliseconds{5U};
  OwnedIo io{domain, uart, t.timing, cfg};
  KB_REQUIRE(io.Create().IsOk());
  const kb_it::OwnerLoop<OwnerBus, OwnedIo> owner{domain, io};
  KB_REQUIRE(io.EnsureStarted(kWait).IsOk());
  KB_CHECK(dx::SetTorque(io, kS1, true, kWait).IsOk());
  KB_CHECK(dx::SetGoalPosition(io, kS1, 1500, kWait).IsOk());
  const auto snap = io.Sample(kWait);
  KB_REQUIRE(snap.IsOk());
  KB_CHECK_EQ(snap.Value().value.feedback[0].position, 1111);
  KB_CHECK(io.Latest().IsOk());
  KB_CHECK(io.Liveness().Beats() > 0U);
  KB_CHECK(io.Stop(kWait).IsOk());
  KB_CHECK(ServoAt(uart.bus, 1U).TorqueOn());
  KB_CHECK_EQ(ServoAt(uart.bus, 1U).Dword(116), 1500);
  KB_CHECK(kb_it::StateOf(io) == HealthState::Unknown);
}

} // namespace
