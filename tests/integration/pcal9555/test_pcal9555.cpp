// pcal9555 through kb::handlers::pcal9555: start-up, the app API, error mapping to the health
// record, recovery, and both thread models, over a kb::iface::I2cBus backed by the driver's own
// protocol model (components/drivers/driver-pcal9555/tests/mock_pcal9555_bus.hpp).
// SPDX-License-Identifier: MIT Copyright (c) 2026 The kb-core authors
#include <cstdint>
#include <span>

#include "binding_support.hpp"
#include "kb/handlers/bindings/pcal9555.hpp"
#include "kb_test.hpp"
#include "mock_pcal9555_bus.hpp"

namespace {

using kb::handlers::HealthState;

/// kb::iface::I2cBus over the driver's chip model and its fault injection.
class ModelBus {
public:
  pcal9555_test::MockBusState state{};

  [[nodiscard]] kb::Status EnsureReady() noexcept {
    return state.bus_up_ok ? kb::Status::Ok : kb::Status::NotReady;
  }
  [[nodiscard]] kb::Status Write(uint8_t address, std::span<const uint8_t> tx) noexcept {
    return outcome(state.DoWrite(address, tx.data(), tx.size()));
  }
  [[nodiscard]] kb::Status WriteRead(uint8_t address, std::span<const uint8_t> tx,
                                     std::span<uint8_t> rx) noexcept {
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
  [[nodiscard]] kb::Status outcome(bool ok) noexcept {
    kb::Status s = kb::Status::Ok;
    last_ = kb::iface::I2cFailure::None;
    if (!ok) {
      switch (state.last_failure) {
      case pcal9555::I2cFailure::AddressNack:
        last_ = kb::iface::I2cFailure::AddressNack;
        s = kb::Status::NoDevice;
        break;
      case pcal9555::I2cFailure::DataNack:
        last_ = kb::iface::I2cFailure::DataNack;
        s = kb::Status::Nack;
        break;
      case pcal9555::I2cFailure::Other:
        last_ = kb::iface::I2cFailure::Bus;
        s = kb::Status::BusError;
        break;
      }
    }
    return s;
  }
  kb::iface::I2cFailure last_{kb::iface::I2cFailure::None};
};
static_assert(kb::iface::I2cBus<ModelBus>);

using Handler = kb::handlers::pcal9555::Handler<ModelBus>;
using GuardedBus = kb::handlers::BusDomain<ModelBus, kb::handlers::ThreadModel::Guarded>;
using OwnerBus = kb::handlers::BusDomain<ModelBus, kb::handlers::ThreadModel::Owner>;
using GuardedIo = kb::handlers::Guarded<Handler, GuardedBus>;
using OwnedIo = kb::handlers::OwnerAccess<Handler, OwnerBus>;

constexpr kb::Milliseconds kWait{200U};

kb::handlers::pcal9555::Config OutputsLow() {
  kb::handlers::pcal9555::Config c{};
  c.chip.input_mask = 0xFF00U; // port 0 outputs, port 1 inputs
  c.chip.output_high_mask = 0x0000U;
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
  explicit GuardedRig(const kb::handlers::pcal9555::Config& cfg) : io(domain, bus, t.timing, cfg) {
    static_cast<void>(domain.Create());
    static_cast<void>(io.Create());
  }
};

uint16_t OutputRegister(ModelBus& bus) {
  uint8_t rx[2] = {0U, 0U};
  static_cast<void>(bus.state.chip.WriteRead(pcal9555_test::chip::kOutputPort0, rx, 2U));
  return static_cast<uint16_t>(rx[0] | (rx[1] << 8U));
}

KB_TEST("pcal9555: guarded start runs the one sync stage and the handler is Ready") {
  GuardedRig r{OutputsLow()};
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Unknown);
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  const kb::Result<kb::handlers::StartupReport> report = r.io.Startup();
  KB_REQUIRE(report.IsOk());
  KB_CHECK(report.Value().complete);
  KB_CHECK_EQ(report.Value().stages, 1U);
  KB_CHECK_EQ(report.Value().kinds,
              kb::handlers::stage::kIdentify | kb::handlers::stage::kConfigure);
  KB_CHECK_EQ(OutputRegister(r.bus), 0x0000U); // the configuration reached the chip
  KB_CHECK(r.io.EnsureStarted(kWait).IsOk());  // idempotent
  KB_CHECK_EQ(r.io.Health().Value().starts, 1U);
}

KB_TEST("pcal9555: calls before a start send nothing and return NotReady") {
  GuardedRig r{OutputsLow()};
  const int before = r.bus.state.transfers;
  KB_CHECK(kb::handlers::pcal9555::ReadInputs(r.io, kWait).Error() == kb::Status::NotReady);
  KB_CHECK(r.io.Sample(kWait).Error() == kb::Status::NotReady);
  KB_CHECK_EQ(r.bus.state.transfers, before);
}

KB_TEST("pcal9555: an absent chip fails the start with NoDevice and the driver's code; a retry "
        "starts again") {
  GuardedRig r{OutputsLow()};
  r.bus.state.line = pcal9555_test::Line::FloatHigh;
  const kb::Result<void> first = r.io.EnsureStarted(kWait);
  KB_CHECK(first.Error() == kb::Status::NoDevice);
  KB_CHECK_EQ(first.Detail(), kb::handlers::DetailOf(pcal9555::Error::NoDevice));
  const kb::handlers::HealthRecord h = r.io.Health().Value();
  KB_CHECK(h.state == HealthState::Faulted);
  KB_CHECK_EQ(h.start_failures, 1U);
  KB_CHECK(h.last_status == kb::Status::NoDevice);
  const kb::handlers::StartupReport rep = r.io.Startup().Value();
  KB_CHECK(!rep.complete);
  KB_CHECK(rep.status == kb::Status::NoDevice);
  r.bus.state.line = pcal9555_test::Line::Ok;
  KB_CHECK(r.io.EnsureStarted(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
}

KB_TEST("pcal9555: outputs and inputs through the app API reach the chip model") {
  GuardedRig r{OutputsLow()};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  KB_CHECK(kb::handlers::pcal9555::WriteOutputs(r.io, 0x00A5U, kWait).IsOk());
  KB_CHECK_EQ(OutputRegister(r.bus) & 0x00FFU, 0x00A5U);
  KB_CHECK(kb::handlers::pcal9555::SetOutputs(r.io, 0x0002U, true, kWait).IsOk());
  KB_CHECK_EQ(OutputRegister(r.bus) & 0x00FFU, 0x00A7U);
  r.bus.state.chip.SetExternal(0x5A00U);
  const kb::Result<uint16_t> in = kb::handlers::pcal9555::ReadInputs(r.io, kWait);
  KB_REQUIRE(in.IsOk());
  KB_CHECK_EQ(in.Value() & 0xFF00U, 0x5A00U);
  const auto snap = r.io.Sample(kWait);
  KB_REQUIRE(snap.IsOk());
  KB_CHECK_EQ(snap.Value().value.inputs & 0xFF00U, 0x5A00U);
  KB_CHECK_EQ(snap.Value().value.outputs & 0x00FFU, 0x00A7U);
  KB_CHECK(r.io.Latest().IsOk());
}

KB_TEST("pcal9555: bus failures degrade, the threshold faults, Recover() restores Ready") {
  GuardedRig r{OutputsLow()};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  r.bus.state.fail_transfer = r.bus.state.transfers; // the next transaction fails on the bus
  const kb::Result<uint16_t> bad = kb::handlers::pcal9555::ReadInputs(r.io, kWait);
  KB_CHECK(bad.Error() == kb::Status::BusError);
  KB_CHECK_EQ(bad.Detail(), kb::handlers::DetailOf(pcal9555::Error::Bus));
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Degraded);
  KB_CHECK(kb::handlers::pcal9555::ReadInputs(r.io, kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  r.bus.state.line = pcal9555_test::Line::StuckLow;
  KB_CHECK(!kb::handlers::pcal9555::ReadInputs(r.io, kWait).IsOk());
  KB_CHECK(!kb::handlers::pcal9555::ReadInputs(r.io, kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Faulted);
  const int before = r.bus.state.transfers;
  KB_CHECK(kb::handlers::pcal9555::ReadInputs(r.io, kWait).Error() == kb::Status::NotReady);
  KB_CHECK_EQ(r.bus.state.transfers, before); // reads never restart a faulted chip
  r.bus.state.line = pcal9555_test::Line::Ok;
  KB_CHECK(r.io.RequestRecovery(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  KB_CHECK_EQ(r.io.Health().Value().recoveries, 1U);
}

KB_TEST("pcal9555: a power-on reset of the chip is fatal at once") {
  GuardedRig r{OutputsLow()};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  r.bus.state.chip.PowerOn();
  kb::handlers::pcal9555::Command c{};
  c.op = kb::handlers::pcal9555::Op::CheckForReset;
  const auto res = r.io.Request(c, kWait);
  KB_CHECK(res.Error() == kb::Status::NotReady);
  KB_CHECK_EQ(res.Detail(), kb::handlers::DetailOf(pcal9555::Error::DeviceReset));
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Faulted);
  KB_CHECK(r.io.RequestRecovery(kWait).IsOk());
  KB_CHECK_EQ(OutputRegister(r.bus), 0x0000U); // the configuration is written again
}

KB_TEST("pcal9555: owner model: the owner task starts, samples and serves requests") {
  kb_it::Timing t{};
  ModelBus bus{};
  OwnerBus domain{bus};
  kb::handlers::pcal9555::Config cfg = OutputsLow();
  cfg.handler.sample_period = kb::Milliseconds{2U};
  OwnedIo io{domain, bus, t.timing, cfg};
  KB_REQUIRE(io.Create().IsOk());
  bus.state.chip.SetExternal(0xC300U); // before the owner task runs: the model is not thread-safe
  const kb_it::OwnerLoop<OwnerBus, OwnedIo> owner{domain, io};
  KB_REQUIRE(io.EnsureStarted(kWait).IsOk());
  KB_CHECK(kb::handlers::pcal9555::WriteOutputs(io, 0x003CU, kWait).IsOk());
  const auto snap = io.Sample(kWait);
  KB_REQUIRE(snap.IsOk());
  KB_CHECK_EQ(snap.Value().value.inputs & 0xFF00U, 0xC300U);
  KB_CHECK_EQ(snap.Value().value.outputs & 0x00FFU, 0x003CU);
  KB_CHECK(io.Liveness().Beats() > 0U);
  KB_CHECK(io.Stop(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(io) == HealthState::Unknown);
}

} // namespace
