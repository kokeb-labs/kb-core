// tmc5160 through kb::handlers::tmc5160: start-up, the app API, error mapping to the health record,
// recovery, the control pins and both thread models, over a kb::iface::SpiDevice and a
// kb::iface::UartPort backed by the driver's own chip model
// (components/drivers/driver-tmc5160/tests/mock_tmc5160_bus.hpp). SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

#include "binding_support.hpp"
#include "kb/handlers/bindings/tmc5160.hpp"
#include "kb/interfaces/host/fake_pins.hpp"
#include "kb_test.hpp"
#include "mock_tmc5160_bus.hpp"

namespace {

using kb::handlers::HealthState;
namespace tb = kb::handlers::tmc5160;

/// The chip model of one TMC5160 on its wire, as a declared motor and board expect it.
struct Model {
  tmc5160_test::Wire wire{};
  Model() {
    wire.ChipAt().k.rsense_mohm = 100U;
    wire.xfer_us = 100U; // the chip runs on while bytes are on the line
  }
  tmc5160_test::Chip& Chip() {
    return wire.ChipAt();
  }
};

/// kb::iface::SpiDevice over the model's SPI datagrams and fault injection.
class ModelSpi {
public:
  explicit ModelSpi(Model& m) : m_(m) {}
  [[nodiscard]] kb::Status EnsureReady() noexcept {
    return m_.wire.faults.bus_up_ok ? kb::Status::Ok : kb::Status::NotReady;
  }
  [[nodiscard]] kb::Status Transfer(std::span<const uint8_t> tx, std::span<uint8_t> rx) noexcept {
    return m_.wire.Spi(tx.data(), rx.data(), tx.size()) ? kb::Status::Ok : kb::Status::BusError;
  }

private:
  Model& m_;
};
static_assert(kb::iface::SpiDevice<ModelSpi>);

/// kb::iface::UartPort over the model's UART nodes: a write runs the exchange, the reply waits in a
/// FIFO.
class ModelUart {
public:
  explicit ModelUart(Model& m) : m_(m) {}
  [[nodiscard]] kb::Status EnsureReady() noexcept {
    return m_.wire.faults.bus_up_ok ? kb::Status::Ok : kb::Status::NotReady;
  }
  [[nodiscard]] kb::Status Write(std::span<const uint8_t> tx) noexcept {
    const std::size_t expect = (tx.size() == 4U) ? 8U : 0U; // a read request is answered by 8 bytes
    std::array<uint8_t, 8> reply{};
    const tmc5160::BusStatus s =
        m_.wire.Uart(tx.data(), tx.size(), reply.data(), expect, 1'000'000U);
    if ((s == tmc5160::BusStatus::Ok) && (expect != 0U)) {
      for (std::size_t i = 0U; i < expect; ++i) {
        fifo_[i] = reply[i];
      }
      have_ = expect;
      taken_ = 0U;
    }
    return (s == tmc5160::BusStatus::Fault) ? kb::Status::BusError : kb::Status::Ok;
  }
  [[nodiscard]] kb::Result<std::size_t> Read(std::span<uint8_t> rx,
                                             kb::Milliseconds timeout) noexcept {
    static_cast<void>(timeout);
    std::size_t n = 0U;
    while ((n < rx.size()) && (taken_ < have_)) {
      rx[n] = fifo_[taken_];
      ++n;
      ++taken_;
    }
    return (n == 0U) ? kb::Result<std::size_t>::Failure(kb::Status::Timeout)
                     : kb::Result<std::size_t>::Success(n);
  }
  [[nodiscard]] kb::Status FlushInput() noexcept {
    have_ = 0U;
    taken_ = 0U;
    return kb::Status::Ok;
  }

private:
  Model& m_;
  std::array<uint8_t, 8> fifo_{};
  std::size_t have_{0U};
  std::size_t taken_{0U};
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

constexpr kb::Milliseconds kWait{500U};

/// A declared 1 A motor on 100 mOhm sense resistors and 24 V, with a fast ramp.
tb::Config Axis() {
  tb::Config c{};
  c.chip = tmc5160::MakeConfig(tmc5160::Motor{.rated_rms_ma = 1000U, .coil_resistance_mohm = 6000U},
                               tmc5160::Board{.rsense_mohm = 100U, .supply_mv = 24'000U});
  c.chip.ramp.amax = 20'000U;
  c.chip.ramp.dmax = 20'000U;
  c.chip.ramp.a1 = 20'000U;
  c.chip.ramp.d1 = 20'000U;
  c.chip.ramp.vmax = 100'000U;
  c.chip.ramp.v1 = 50'000U;
  c.handler.health.fault_after = 2U;
  c.handler.recovery.backoff = kb::Milliseconds{1U};
  return c;
}

/// A guarded SPI handler on its own bus, created.
struct SpiRig {
  kb_it::Timing t{};
  kb::handlers::HookTiming timing{t.timing};
  Model model{};
  ModelSpi spi{model};
  SpiGuarded domain{spi};
  SpiIo io;
  explicit SpiRig(const tb::Config& cfg) : io(domain, spi, timing, cfg) {
    static_cast<void>(domain.Create());
    static_cast<void>(io.Create());
  }
};

KB_TEST("tmc5160: guarded SPI start runs the one sync stage and writes the configuration") {
  SpiRig r{Axis()};
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Unknown);
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  const kb::handlers::StartupReport rep = r.io.Startup().Value();
  KB_CHECK(rep.complete);
  KB_CHECK_EQ(rep.stages, 1U);
  KB_CHECK_EQ(rep.kinds, kb::handlers::stage::kIdentify | kb::handlers::stage::kConfigure);
  KB_CHECK(r.model.Chip().writes > 10U); // every register from the configuration
  const int before = r.model.wire.transfers;
  KB_CHECK(r.io.EnsureStarted(kWait).IsOk()); // idempotent, no traffic
  KB_CHECK_EQ(r.model.wire.transfers, before);
  KB_CHECK_EQ(r.io.Health().Value().starts, 1U);
}

KB_TEST("tmc5160: calls before a start send nothing and return NotReady") {
  SpiRig r{Axis()};
  const int before = r.model.wire.transfers;
  KB_CHECK(tb::Enable(r.io, kWait).Error() == kb::Status::NotReady);
  KB_CHECK(tb::ReadPosition(r.io, kWait).Error() == kb::Status::NotReady);
  KB_CHECK(r.io.Sample(kWait).Error() == kb::Status::NotReady);
  KB_CHECK_EQ(r.model.wire.transfers, before);
}

KB_TEST("tmc5160: a wrong chip version fails the start with NoDevice and the driver's code; a "
        "retry starts again") {
  SpiRig r{Axis()};
  r.model.Chip().k.version_byte = 0x21U;
  const kb::Result<void> first = r.io.EnsureStarted(kWait);
  KB_CHECK(first.Error() == kb::Status::NoDevice);
  KB_CHECK_EQ(first.Detail(), kb::handlers::DetailOf(tmc5160::Error::DeviceIdMismatch));
  const kb::handlers::HealthRecord h = r.io.Health().Value();
  KB_CHECK(h.state == HealthState::Faulted);
  KB_CHECK_EQ(h.start_failures, 1U);
  KB_CHECK(!r.io.Startup().Value().complete);
  r.model.Chip().k.version_byte = 0x30U;
  KB_CHECK(r.io.EnsureStarted(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
}

KB_TEST("tmc5160: enable, current, move, position, events, stop and disable reach the chip model") {
  SpiRig r{Axis()};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  KB_CHECK(tb::Enable(r.io, kWait).IsOk());
  const kb::Result<uint32_t> run = tb::SetCurrent(r.io, 800U, 200U, kWait);
  KB_REQUIRE(run.IsOk());
  KB_CHECK(run.Value() <= 800U);
  KB_CHECK(run.Value() > 600U);
  KB_CHECK(tb::MoveTo(r.io, 20'000, kWait).IsOk());
  r.model.wire.Delay(2'000'000U); // the ramp runs in the chip
  KB_CHECK_EQ(r.model.Chip().Xactual(), 20'000);
  const kb::Result<int32_t> pos = tb::ReadPosition(r.io, kWait);
  KB_REQUIRE(pos.IsOk());
  KB_CHECK_EQ(pos.Value(), 20'000);
  tb::Command reached{};
  reached.op = tb::Op::PositionReached;
  const kb::Result<tb::Reply> at = r.io.Request(reached, kWait);
  KB_REQUIRE(at.IsOk());
  KB_CHECK(at.Value().flag);
  const auto snap = r.io.Sample(kWait);
  KB_REQUIRE(snap.IsOk());
  KB_CHECK_EQ(snap.Value().value.status.position, 20'000);
  KB_CHECK(snap.Value().value.enabled);
  KB_CHECK(tb::ServiceEvents(r.io, kWait).IsOk());
  KB_CHECK(tb::RunVelocity(r.io, -30'000, kWait).IsOk());
  r.model.wire.Delay(200'000U);
  KB_CHECK(r.model.Chip().Xactual() < 20'000);
  KB_CHECK(tb::Stop(r.io, tmc5160::StopKind::Ramp, kWait).IsOk());
  KB_CHECK(tb::Disable(r.io, kWait).IsOk());
  KB_CHECK(r.io.Latest().IsOk());
}

KB_TEST("tmc5160: bus failures degrade, the threshold faults, Recover() restores Ready") {
  SpiRig r{Axis()};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  r.model.wire.faults.fail_at = r.model.wire.transfers; // the next transfer fails
  const kb::Result<int32_t> bad = tb::ReadPosition(r.io, kWait);
  KB_CHECK(bad.Error() == kb::Status::BusError);
  KB_CHECK_EQ(bad.Detail(), kb::handlers::DetailOf(tmc5160::Error::Bus));
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Degraded);
  KB_CHECK(tb::ReadPosition(r.io, kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  r.model.wire.faults.fail_from = r.model.wire.transfers; // every transfer from now on fails
  KB_CHECK(!tb::ReadPosition(r.io, kWait).IsOk());
  KB_CHECK(!tb::ReadPosition(r.io, kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Faulted);
  const int before = r.model.wire.transfers;
  KB_CHECK(tb::ReadPosition(r.io, kWait).Error() == kb::Status::NotReady);
  KB_CHECK_EQ(r.model.wire.transfers, before); // reads never restart a faulted chip
  r.model.wire.faults.fail_from = -1;
  KB_CHECK(r.io.RequestRecovery(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  KB_CHECK_EQ(r.io.Health().Value().recoveries, 1U);
}

KB_TEST("tmc5160: a chip reset is fatal at once and the recovery writes the configuration again") {
  SpiRig r{Axis()};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  r.model.Chip().PowerOn(); // brown-out: the write-only registers are lost
  tb::Command c{};
  c.op = tb::Op::ClearFaults;
  kb::Result<tb::Reply> res = r.io.Request(c, kWait);
  if (res.IsOk()) {
    res = r.io.Request(c, kWait); // the status byte shows the reset one transfer late
  }
  KB_CHECK(res.Error() == kb::Status::NotReady);
  KB_CHECK_EQ(res.Detail(), kb::handlers::DetailOf(tmc5160::Error::UnexpectedReset));
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Faulted);
  const uint32_t writes = r.model.Chip().writes;
  KB_CHECK(r.io.RequestRecovery(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  KB_CHECK(r.model.Chip().writes > (writes + 10U));
}

KB_TEST("tmc5160: the UART link starts, moves and reads back through the same API") {
  kb_it::Timing t{};
  const kb::handlers::HookTiming timing = t.timing;
  Model model{};
  ModelUart uart{model};
  UartGuarded domain{uart};
  UartIo io{domain, uart, timing, Axis()};
  KB_REQUIRE(domain.Create().IsOk());
  KB_REQUIRE(io.Create().IsOk());
  KB_REQUIRE(io.EnsureStarted(kWait).IsOk());
  KB_CHECK(tb::Enable(io, kWait).IsOk());
  KB_CHECK(tb::MoveTo(io, 5'000, kWait).IsOk());
  model.wire.Delay(2'000'000U);
  const kb::Result<int32_t> pos = tb::ReadPosition(io, kWait);
  KB_REQUIRE(pos.IsOk());
  KB_CHECK_EQ(pos.Value(), 5'000);
  model.wire.faults.fail_from = model.wire.transfers; // the line breaks
  const kb::Result<int32_t> bad = tb::ReadPosition(io, kWait);
  KB_CHECK(!bad.IsOk());
  KB_CHECK(kb_it::StateOf(io) == HealthState::Degraded);
  model.wire.faults.fail_from = -1;
  KB_CHECK(tb::ReadPosition(io, kWait).IsOk());
  KB_CHECK(kb_it::StateOf(io) == HealthState::Ready);
}

KB_TEST("tmc5160: with control pins the driver drives DRV_ENN through the HAL's pin") {
  kb_it::Timing t{};
  const kb::handlers::HookTiming timing = t.timing;
  Model model{};
  ModelSpi spi{model};
  SpiGuarded domain{spi};
  kb::iface::host::FakeGpioPin drv_enn{};
  tb::Config cfg = Axis();
  cfg.pins.outputs[tb::kPinDrvEnn] =
      kb::handlers::OutputPin{kb::iface::GpioOutputRef{drv_enn}, true};
  PinIo io{domain, spi, timing, cfg};
  KB_REQUIRE(domain.Create().IsOk());
  KB_REQUIRE(io.Create().IsOk());
  KB_REQUIRE(io.EnsureStarted(kWait).IsOk());
  KB_CHECK(drv_enn.LevelNow() == kb::iface::Level::High); // off after the start (active low)
  KB_CHECK(tb::Enable(io, kWait).IsOk());
  KB_CHECK(drv_enn.LevelNow() == kb::iface::Level::Low);
  KB_CHECK(tb::ReadPosition(io, kWait).IsOk());
  KB_CHECK(tb::Disable(io, kWait).IsOk());
  KB_CHECK(drv_enn.LevelNow() == kb::iface::Level::High);
}

KB_TEST("tmc5160: owner model: the owner task starts, samples and serves requests") {
  kb_it::Timing t{};
  const kb::handlers::HookTiming timing = t.timing;
  Model model{};
  ModelSpi spi{model};
  SpiOwner domain{spi};
  tb::Config cfg = Axis();
  cfg.handler.sample_period = kb::Milliseconds{2U};
  OwnedIo io{domain, spi, timing, cfg};
  KB_REQUIRE(io.Create().IsOk());
  const kb_it::OwnerLoop<SpiOwner, OwnedIo> owner{domain, io};
  KB_REQUIRE(io.EnsureStarted(kWait).IsOk());
  KB_CHECK(tb::Enable(io, kWait).IsOk());
  KB_CHECK(tb::MoveTo(io, 1'000, kWait).IsOk());
  const auto snap = io.Sample(kWait);
  KB_REQUIRE(snap.IsOk());
  KB_CHECK(snap.Value().value.enabled);
  KB_CHECK(tb::Stop(io, tmc5160::StopKind::Ramp, kWait).IsOk());
  KB_CHECK(io.Liveness().Beats() > 0U);
  KB_CHECK(io.Stop(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(io) == HealthState::Unknown);
}

} // namespace
