// tmc9660 through kb::handlers::tmc9660: start-up, the app API, error mapping to the health record,
// recovery, register mode, the control pins and both thread models, over a kb::iface::SpiDevice and
// a kb::iface::UartPort backed by the driver's own chip model
// (components/drivers/driver-tmc9660/tests/mock_tmc9660_bus.hpp). SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

#include "binding_support.hpp"
#include "kb/handlers/bindings/tmc9660.hpp"
#include "kb/interfaces/host/fake_pins.hpp"
#include "kb_test.hpp"
#include "mock_tmc9660_bus.hpp"

namespace {

using kb::handlers::HealthState;
namespace tb = kb::handlers::tmc9660;
using tmc9660_test::Phase;

/// kb::iface::SpiDevice over the model's SPI transfers and fault injection.
class ModelSpi {
public:
  explicit ModelSpi(tmc9660_test::BusState& s) : s_(s) {}
  [[nodiscard]] kb::Status EnsureReady() noexcept {
    ++s_.bus_up_calls;
    return s_.bus_up_ok ? kb::Status::Ok : kb::Status::NotReady;
  }
  [[nodiscard]] kb::Status Transfer(std::span<const uint8_t> tx, std::span<uint8_t> rx) noexcept {
    return s_.Spi(tx.data(), rx.data(), tx.size()) ? kb::Status::Ok : kb::Status::BusError;
  }

private:
  tmc9660_test::BusState& s_;
};
static_assert(kb::iface::SpiDevice<ModelSpi>);

/// kb::iface::UartPort over the model's UART: a write is one request (its reply waits in the
/// model's receive FIFO), a read takes what the FIFO holds.
class ModelUart {
public:
  explicit ModelUart(tmc9660_test::BusState& s) : s_(s) {}
  [[nodiscard]] kb::Status EnsureReady() noexcept {
    ++s_.bus_up_calls;
    return s_.bus_up_ok ? kb::Status::Ok : kb::Status::NotReady;
  }
  [[nodiscard]] kb::Status Write(std::span<const uint8_t> tx) noexcept {
    const tmc9660::BusStatus st = s_.Uart(tx.data(), tx.size(), nullptr, 0U);
    return (st == tmc9660::BusStatus::Fault) ? kb::Status::BusError : kb::Status::Ok;
  }
  [[nodiscard]] kb::Result<std::size_t> Read(std::span<uint8_t> rx,
                                             kb::Milliseconds timeout) noexcept {
    static_cast<void>(timeout);
    const std::size_t n = (s_.wire.Pending() < rx.size()) ? s_.wire.Pending() : rx.size();
    const bool got = (n != 0U) && s_.wire.Exchange(s_.chip, nullptr, 0U, rx.data(), n);
    return got ? kb::Result<std::size_t>::Success(n)
               : kb::Result<std::size_t>::Failure(kb::Status::Timeout);
  }
  [[nodiscard]] kb::Status FlushInput() noexcept {
    std::array<uint8_t, 64> sink{};
    const std::size_t n = (s_.wire.Pending() < sink.size()) ? s_.wire.Pending() : sink.size();
    static_cast<void>(s_.wire.Exchange(s_.chip, nullptr, 0U, sink.data(), n));
    return kb::Status::Ok;
  }

private:
  tmc9660_test::BusState& s_;
};
static_assert(kb::iface::UartPort<ModelUart>);

using SpiHandler = tb::Handler<ModelSpi>;
using UartHandler = tb::Handler<ModelUart>;
using PinHandler = tb::Handler<ModelSpi, true>;
using SpiGuarded = kb::handlers::BusDomain<ModelSpi, kb::handlers::ThreadModel::Guarded>;
using UartGuarded = kb::handlers::BusDomain<ModelUart, kb::handlers::ThreadModel::Guarded>;
using SpiOwner = kb::handlers::BusDomain<ModelSpi, kb::handlers::ThreadModel::Owner>;
using SpiIo = kb::handlers::Guarded<SpiHandler, SpiGuarded>;
using UartIo = kb::handlers::Guarded<UartHandler, UartGuarded>;
using PinIo = kb::handlers::Guarded<PinHandler, SpiGuarded>;
using OwnedIo = kb::handlers::OwnerAccess<SpiHandler, SpiOwner>;

static_assert(SpiHandler::kBus == kb::handlers::BusKind::Spi);
static_assert(UartHandler::kBus == kb::handlers::BusKind::Uart);

constexpr kb::Milliseconds kWait{2000U};

tb::Config Servo(tmc9660::Mode mode = tmc9660::Mode::Parameter) {
  tb::Config c{};
  c.chip.mode = mode;
  c.chip.spi_gap_us = 200U; // short reply gaps keep the host test quick
  c.handler.health.fault_after = 2U;
  c.handler.recovery.backoff = kb::Milliseconds{1U};
  return c;
}

/// A guarded SPI handler on its own bus, created; the chip boots into `phase` with DRV_EN wired
/// high.
struct SpiRig {
  kb_it::Timing t{};
  kb::handlers::HookTiming timing{t.timing};
  tmc9660_test::BusState state{};
  ModelSpi spi{state};
  SpiGuarded domain{spi};
  SpiIo io;
  SpiRig(Phase phase, const tb::Config& cfg) : io(domain, spi, timing, cfg) {
    state.chip.BootInto(phase);
    state.chip.SetDrvEn(true);
    static_cast<void>(domain.Create());
    static_cast<void>(io.Create());
  }
};

KB_TEST("tmc9660: guarded SPI start in parameter mode runs the one sync stage") {
  SpiRig r{Phase::Parameter, Servo()};
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Unknown);
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  const kb::handlers::StartupReport rep = r.io.Startup().Value();
  KB_CHECK(rep.complete);
  KB_CHECK_EQ(rep.stages, 1U);
  KB_CHECK_EQ(rep.kinds, kb::handlers::stage::kIdentify | kb::handlers::stage::kConfigure);
  const int before = r.state.transfers;
  KB_CHECK(r.io.EnsureStarted(kWait).IsOk()); // idempotent, no traffic
  KB_CHECK_EQ(r.state.transfers, before);
}

KB_TEST("tmc9660: calls before a start send nothing and return NotReady") {
  SpiRig r{Phase::Parameter, Servo()};
  KB_CHECK(tb::SetTargetVelocity(r.io, 10, kWait).Error() == kb::Status::NotReady);
  KB_CHECK(tb::GetParam(r.io, tmc9660::Param::ACTUAL_VELOCITY, kWait).Error() ==
           kb::Status::NotReady);
  KB_CHECK(r.io.Sample(kWait).Error() == kb::Status::NotReady);
  KB_CHECK_EQ(r.state.transfers, 0);
  KB_CHECK_EQ(r.state.bus_up_calls, 0);
}

KB_TEST("tmc9660: a wrong chip ID fails the start with NoDevice and the driver's code; a retry "
        "starts again") {
  SpiRig r{Phase::Parameter, Servo()};
  r.state.chip.chip_id = 0x12345678U;
  const kb::Result<void> first = r.io.EnsureStarted(kWait);
  KB_CHECK(first.Error() == kb::Status::NoDevice);
  KB_CHECK_EQ(first.Detail(), kb::handlers::DetailOf(tmc9660::Error::DeviceIdMismatch));
  const kb::handlers::HealthRecord h = r.io.Health().Value();
  KB_CHECK(h.state == HealthState::Faulted);
  KB_CHECK_EQ(h.start_failures, 1U);
  KB_CHECK(!r.io.Startup().Value().complete);
  r.state.chip.chip_id = 0x544D0001U;
  KB_CHECK(r.io.EnsureStarted(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
}

KB_TEST("tmc9660: drive, targets, parameters, chip ID and the snapshot reach the chip model") {
  SpiRig r{Phase::Parameter, Servo()};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  KB_CHECK(tb::EnableDrive(r.io, 2U, kWait).IsOk());
  KB_CHECK_EQ(tb::GetParam(r.io, tmc9660::Param::COMMUTATION_MODE, kWait).ValueOr(-1), 2);
  KB_CHECK(tb::SetTargetVelocity(r.io, 1000, kWait).IsOk());
  KB_CHECK_EQ(tb::GetParam(r.io, tmc9660::Param::TARGET_VELOCITY, kWait).ValueOr(-1), 1000);
  KB_CHECK(tb::SetTargetPosition(r.io, -5000, kWait).IsOk());
  KB_CHECK_EQ(tb::GetParam(r.io, tmc9660::Param::TARGET_POSITION, kWait).ValueOr(0), -5000);
  KB_CHECK(tb::SetParam(r.io, tmc9660::Param::MAX_TORQUE, 2000, kWait).IsOk());
  KB_CHECK_EQ(tb::GetParam(r.io, tmc9660::Param::MAX_TORQUE, kWait).ValueOr(0), 2000);
  tb::Command id{};
  id.op = tb::Op::GetChipId;
  const kb::Result<tb::Reply> chip = r.io.Request(id, kWait);
  KB_REQUIRE(chip.IsOk());
  KB_CHECK_EQ(chip.Value().word, r.state.chip.chip_id);
  tb::Command stop{};
  stop.op = tb::Op::MotorStop;
  KB_CHECK(r.io.Request(stop, kWait).IsOk());
  const auto snap = r.io.Sample(kWait);
  KB_REQUIRE(snap.IsOk());
  KB_CHECK(snap.Value().value.mode == tmc9660::Mode::Parameter);
  KB_CHECK(tb::DisableDrive(r.io, kWait).IsOk());
  KB_CHECK_EQ(tb::GetParam(r.io, tmc9660::Param::COMMUTATION_MODE, kWait).ValueOr(-1), 0);
}

KB_TEST("tmc9660: bus failures degrade, the threshold faults, Recover() restores Ready") {
  SpiRig r{Phase::Parameter, Servo()};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  r.state.fail_transfer = r.state.transfers; // the next transfer fails
  r.state.fail_span = 1;
  const kb::Result<int64_t> bad = tb::GetParam(r.io, tmc9660::Param::ACTUAL_VELOCITY, kWait);
  KB_CHECK(bad.Error() == kb::Status::BusError);
  KB_CHECK_EQ(bad.Detail(), kb::handlers::DetailOf(tmc9660::Error::Bus));
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Degraded);
  KB_CHECK(tb::GetParam(r.io, tmc9660::Param::ACTUAL_VELOCITY, kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  r.state.fail_transfer = r.state.transfers; // every transfer from now on fails
  r.state.fail_span = 1000000;
  KB_CHECK(!tb::GetParam(r.io, tmc9660::Param::ACTUAL_VELOCITY, kWait).IsOk());
  KB_CHECK(!tb::GetParam(r.io, tmc9660::Param::ACTUAL_VELOCITY, kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Faulted);
  const int before = r.state.transfers;
  KB_CHECK(tb::GetParam(r.io, tmc9660::Param::ACTUAL_VELOCITY, kWait).Error() ==
           kb::Status::NotReady);
  KB_CHECK_EQ(r.state.transfers, before); // reads never restart a faulted chip
  r.state.fail_transfer = -1;
  KB_CHECK(r.io.RequestRecovery(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  KB_CHECK_EQ(r.io.Health().Value().recoveries, 1U);
}

KB_TEST("tmc9660: a chip restart the driver detects is fatal at once; the recovery adopts the chip "
        "again") {
  SpiRig r{Phase::Parameter, Servo()};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  r.state.reset_at_transfer = r.state.transfers; // the chip restarts before the next request
  tb::Command svc{};
  svc.op = tb::Op::Service;
  kb::Result<tb::Reply> res = r.io.Request(svc, kWait);
  if (res.IsOk()) {
    res = r.io.Request(svc, kWait);
  }
  KB_CHECK(res.Error() == kb::Status::NotReady);
  KB_CHECK_EQ(res.Detail(), kb::handlers::DetailOf(tmc9660::Error::UnexpectedReset));
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Faulted);
  KB_CHECK(r.io.RequestRecovery(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  KB_CHECK(tb::GetParam(r.io, tmc9660::Param::ACTUAL_VELOCITY, kWait).IsOk());
}

KB_TEST(
    "tmc9660: register mode reads registers; a parameter call there is Unsupported (WrongMode)") {
  SpiRig r{Phase::Register, Servo(tmc9660::Mode::Register)};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  tb::Command rd{};
  rd.op = tb::Op::ReadReg;
  rd.reg = tmc9660::regs::MCC_INFO_CHIP;
  const kb::Result<tb::Reply> chip = r.io.Request(rd, kWait);
  KB_REQUIRE(chip.IsOk());
  KB_CHECK(chip.Value().word != 0U);
  const kb::Result<int64_t> wrong = tb::GetParam(r.io, tmc9660::Param::ACTUAL_VELOCITY, kWait);
  KB_CHECK(wrong.Error() == kb::Status::Unsupported);
  KB_CHECK_EQ(wrong.Detail(), kb::handlers::DetailOf(tmc9660::Error::WrongMode));
  const auto snap = r.io.Sample(kWait);
  KB_REQUIRE(snap.IsOk());
  KB_CHECK(snap.Value().value.mode == tmc9660::Mode::Register);
}

KB_TEST("tmc9660: the UART link starts and serves the same API") {
  kb_it::Timing t{};
  const kb::handlers::HookTiming timing = t.timing;
  tmc9660_test::BusState state{};
  state.chip.BootInto(Phase::Parameter);
  state.chip.SetDrvEn(true);
  ModelUart uart{state};
  UartGuarded domain{uart};
  UartIo io{domain, uart, timing, Servo()};
  KB_REQUIRE(domain.Create().IsOk());
  KB_REQUIRE(io.Create().IsOk());
  KB_REQUIRE(io.EnsureStarted(kWait).IsOk());
  KB_CHECK(tb::SetTargetVelocity(io, -250, kWait).IsOk());
  KB_CHECK_EQ(tb::GetParam(io, tmc9660::Param::TARGET_VELOCITY, kWait).ValueOr(0), -250);
  KB_CHECK(io.Sample(kWait).IsOk());
}

KB_TEST("tmc9660: with control pins the driver drives DRV_EN through the HAL's pin") {
  kb_it::Timing t{};
  const kb::handlers::HookTiming timing = t.timing;
  tmc9660_test::BusState state{};
  state.chip.BootInto(Phase::Parameter);
  ModelSpi spi{state};
  SpiGuarded domain{spi};
  kb::iface::host::FakeGpioPin drv_en{};
  kb::iface::host::FakeGpioPin faultn{};
  faultn.Drive(kb::iface::Level::High); // no fault
  tb::Config cfg = Servo();
  cfg.pins.outputs[tb::kPinDrvEn] =
      kb::handlers::OutputPin{kb::iface::GpioOutputRef{drv_en}, false};
  cfg.pins.inputs[tb::kPinFaultn] = kb::handlers::InputPin{kb::iface::GpioInputRef{faultn}, true};
  PinIo io{domain, spi, timing, cfg};
  KB_REQUIRE(domain.Create().IsOk());
  KB_REQUIRE(io.Create().IsOk());
  KB_REQUIRE(io.EnsureStarted(kWait).IsOk());
  KB_CHECK(tb::EnableDrive(io, 2U, kWait).IsOk());
  KB_CHECK(drv_en.LevelNow() == kb::iface::Level::High);
  KB_CHECK(tb::DisableDrive(io, kWait).IsOk());
  KB_CHECK(drv_en.LevelNow() == kb::iface::Level::Low);
  KB_CHECK(io.Sample(kWait).IsOk()); // Service() reads FAULTN high: no fault
}

KB_TEST("tmc9660: owner model: the owner task starts, samples and serves requests") {
  kb_it::Timing t{};
  const kb::handlers::HookTiming timing = t.timing;
  tmc9660_test::BusState state{};
  state.chip.BootInto(Phase::Parameter);
  state.chip.SetDrvEn(true);
  ModelSpi spi{state};
  SpiOwner domain{spi};
  tb::Config cfg = Servo();
  cfg.handler.sample_period = kb::Milliseconds{5U};
  OwnedIo io{domain, spi, timing, cfg};
  KB_REQUIRE(io.Create().IsOk());
  const kb_it::OwnerLoop<SpiOwner, OwnedIo> owner{domain, io};
  KB_REQUIRE(io.EnsureStarted(kWait).IsOk());
  KB_CHECK(tb::SetTargetVelocity(io, 321, kWait).IsOk());
  KB_CHECK_EQ(tb::GetParam(io, tmc9660::Param::TARGET_VELOCITY, kWait).ValueOr(0), 321);
  const auto snap = io.Sample(kWait);
  KB_REQUIRE(snap.IsOk());
  KB_CHECK(snap.Value().value.mode == tmc9660::Mode::Parameter);
  KB_CHECK(io.Liveness().Beats() > 0U);
  KB_CHECK(io.Stop(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(io) == HealthState::Unknown);
}

} // namespace
