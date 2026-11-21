// bmp581 through kb::handlers::bmp581: start-up, the app API, error mapping to the health record,
// recovery, both transports (I2C and SPI) and both thread models, over kb::iface::I2cBus and
// kb::iface::SpiDevice adapters backed by the driver's own register model
// (components/drivers/driver-bmp581/tests/mock_bmp581_bus.hpp).
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include <cstdint>
#include <span>

#include "binding_support.hpp"
#include "kb/handlers/bindings/bmp581.hpp"
#include "kb_test.hpp"
#include "mock_bmp581_bus.hpp"

namespace {

using kb::handlers::HealthState;
namespace baro = kb::handlers::bmp581;

/// Advances the model's clock with the RTOS clock, so the driver's waits pass in the model too.
class ModelClock {
public:
  void Sync(bmp581_test::MockBusState& state) noexcept {
    const uint64_t now = kb::rtos::Now().Us();
    state.chip.Elapse(now - last_us_);
    last_us_ = now;
  }

private:
  uint64_t last_us_{kb::rtos::Now().Us()}; // the model's clock starts with the object
};

/// kb::iface::I2cBus over the driver's register model and its fault injection.
class ModelI2c {
public:
  bmp581_test::MockBusState state{};

  [[nodiscard]] kb::Status EnsureReady() noexcept {
    return state.bus_up_ok ? kb::Status::Ok : kb::Status::NotReady;
  }
  [[nodiscard]] kb::Status Write(uint8_t address, std::span<const uint8_t> tx) noexcept {
    clock_.Sync(state);
    return outcome(state.I2cWrite(address, tx.data(), tx.size()));
  }
  [[nodiscard]] kb::Status WriteRead(uint8_t address, std::span<const uint8_t> tx,
                                     std::span<uint8_t> rx) noexcept {
    clock_.Sync(state);
    return outcome(state.I2cWriteRead(address, tx.data(), tx.size(), rx.data(), rx.size()));
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
      case bmp581::I2cFailure::AddressNack:
        last_ = kb::iface::I2cFailure::AddressNack;
        s = kb::Status::NoDevice;
        break;
      case bmp581::I2cFailure::DataNack:
        last_ = kb::iface::I2cFailure::DataNack;
        s = kb::Status::Nack;
        break;
      case bmp581::I2cFailure::Other:
        last_ = kb::iface::I2cFailure::Bus;
        s = kb::Status::BusError;
        break;
      }
    }
    return s;
  }
  ModelClock clock_{};
  kb::iface::I2cFailure last_{kb::iface::I2cFailure::None};
};
static_assert(kb::iface::I2cBus<ModelI2c>);

/// kb::iface::SpiDevice over the same model, four-wire.
class ModelSpi {
public:
  bmp581_test::MockBusState state{};

  [[nodiscard]] kb::Status EnsureReady() noexcept {
    return state.bus_up_ok ? kb::Status::Ok : kb::Status::NotReady;
  }
  [[nodiscard]] kb::Status Transfer(std::span<const uint8_t> tx, std::span<uint8_t> rx) noexcept {
    clock_.Sync(state);
    return state.SpiTransfer(tx.data(), rx.empty() ? nullptr : rx.data(), tx.size())
               ? kb::Status::Ok
               : kb::Status::BusError;
  }

private:
  ModelClock clock_{};
};
static_assert(kb::iface::SpiDevice<ModelSpi>);
static_assert(baro::Handler<ModelI2c>::kBus == kb::handlers::BusKind::I2c);
static_assert(baro::Handler<ModelSpi>::kBus == kb::handlers::BusKind::Spi);

constexpr kb::Milliseconds kWait{300U};

baro::Config Standby() {
  baro::Config c{};
  c.handler.health.fault_after = 2U;
  c.handler.recovery.backoff = kb::Milliseconds{1U};
  return c;
}

/// A guarded handler on its own bus, created.
template <typename Bus>
struct GuardedRig {
  using Domain = kb::handlers::BusDomain<Bus, kb::handlers::ThreadModel::Guarded>;
  kb_it::Timing t{};
  Bus bus{};
  Domain domain{bus};
  kb::handlers::Guarded<baro::Handler<Bus>, Domain> io;
  explicit GuardedRig(const baro::Config& cfg) : io(domain, bus, t.timing, cfg) {
    static_cast<void>(domain.Create());
    static_cast<void>(io.Create());
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

KB_TEST("bmp581: guarded start on I2C runs the one sync stage and the handler is Ready") {
  GuardedRig<ModelI2c> r{Standby()};
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Unknown);
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  const kb::handlers::StartupReport rep = r.io.Startup().Value();
  KB_CHECK(rep.complete);
  KB_CHECK_EQ(rep.stages, 1U);
  KB_CHECK_EQ(rep.kinds, kb::handlers::stage::kIdentify | kb::handlers::stage::kConfigure);
  KB_CHECK_EQ(r.bus.state.chip.soft_resets, 1);
  KB_CHECK(r.io.EnsureStarted(kWait).IsOk()); // idempotent
  KB_CHECK_EQ(r.io.Health().Value().starts, 1U);
}

KB_TEST("bmp581: calls before a start send nothing and return NotReady") {
  GuardedRig<ModelI2c> r{Standby()};
  const int before = r.bus.state.transfers;
  KB_CHECK(baro::MeasureOnce(r.io, kWait).Error() == kb::Status::NotReady);
  KB_CHECK(r.io.Sample(kWait).Error() == kb::Status::NotReady);
  KB_CHECK_EQ(r.bus.state.transfers, before);
}

KB_TEST(
    "bmp581: an absent chip fails with NoDevice, a wrong chip with Fault; a retry starts again") {
  GuardedRig<ModelI2c> r{Standby()};
  r.bus.state.fail_from = 0; // nothing acknowledges the address
  const kb::Result<void> first = r.io.EnsureStarted(kWait);
  KB_CHECK(first.Error() == kb::Status::NoDevice);
  KB_CHECK_EQ(first.Detail(), kb::handlers::DetailOf(bmp581::Error::NoDevice));
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Faulted);
  KB_CHECK(r.io.Startup().Value().status == kb::Status::NoDevice);
  r.bus.state.fail_from = -1;
  r.bus.state.chip.chip_id = 0x51U;
  const kb::Result<void> second = r.io.EnsureStarted(kWait);
  KB_CHECK(second.Error() == kb::Status::Fault);
  KB_CHECK_EQ(second.Detail(), kb::handlers::DetailOf(bmp581::Error::DeviceIdMismatch));
  KB_CHECK_EQ(r.io.Health().Value().start_failures, 2U);
  r.bus.state.chip.chip_id = bmp581_test::chip::kChipIdValue;
  KB_CHECK(r.io.EnsureStarted(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
}

KB_TEST("bmp581: forced samples, the app API and normal mode reach the model") {
  GuardedRig<ModelI2c> r{Standby()};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  r.bus.state.chip.pressure_raw = 101'325 * 64;
  r.bus.state.chip.temperature_raw = 21 * 65536;
  const auto snap = r.io.Sample(kWait);
  KB_REQUIRE(snap.IsOk());
  KB_CHECK(snap.Value().value.forced);
  KB_CHECK_EQ(snap.Value().value.pressure_raw, 101'325 * 64);
  KB_CHECK(snap.Value().value.pressure_pa == 101325.0F);
  KB_CHECK(snap.Value().value.temperature_c == 21.0F);
  const kb::Result<float> pa = baro::ReadPressurePa(r.io, kWait);
  KB_REQUIRE(pa.IsOk());
  KB_CHECK(pa.Value() == 101325.0F);
  KB_CHECK(baro::CheckForReset(r.io, kWait).IsOk());
  baro::Command id{};
  id.op = baro::Op::ReadRegister;
  id.address = 0x01U; // CHIP_ID
  const auto chip_id = r.io.Request(id, kWait);
  KB_REQUIRE(chip_id.IsOk());
  KB_CHECK_EQ(chip_id.Value().raw, 0x50U);
  KB_CHECK(baro::SetPowerMode(r.io, bmp581::PowerMode::Normal, kWait).IsOk());
  kb::rtos::SleepFor(kb::Milliseconds{10U});
  const auto normal = r.io.Sample(kWait);
  KB_REQUIRE(normal.IsOk());
  KB_CHECK(!normal.Value().value.forced);
  KB_CHECK_EQ(normal.Value().value.temperature_raw, 21 * 65536);
  baro::Command irq{};
  irq.op = baro::Op::ReadInterruptStatus;
  KB_CHECK(r.io.Request(irq, kWait).IsOk());
}

KB_TEST(
    "bmp581: a failed transfer and a stuck line degrade, then fault; Recover() restores Ready") {
  GuardedRig<ModelI2c> r{Standby()};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  baro::Command read{};
  read.op = baro::Op::ReadMeasurement;
  r.bus.state.lost_ack_transfer = r.bus.state.transfers; // the next transfer is reported failed
  const auto bad = r.io.Request(read, kWait);
  KB_CHECK(bad.Error() == kb::Status::BusError);
  KB_CHECK_EQ(bad.Detail(), kb::handlers::DetailOf(bmp581::Error::Bus));
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Degraded);
  KB_CHECK(r.io.Sample(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  r.bus.state.line = bmp581_test::LineFault::FloatHigh;
  r.bus.state.line_from = r.bus.state.transfers;
  const auto stuck = r.io.Request(read, kWait);
  KB_CHECK(stuck.Error() == kb::Status::BusError);
  KB_CHECK_EQ(stuck.Detail(), kb::handlers::DetailOf(bmp581::Error::InvalidReply));
  KB_CHECK(!r.io.Request(read, kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Faulted);
  const int before = r.bus.state.transfers;
  KB_CHECK(r.io.Request(read, kWait).Error() == kb::Status::NotReady);
  KB_CHECK_EQ(r.bus.state.transfers, before); // nothing restarts a faulted chip
  r.bus.state.line = bmp581_test::LineFault::None;
  KB_CHECK(r.io.RequestRecovery(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  KB_CHECK_EQ(r.io.Health().Value().recoveries, 1U);
}

KB_TEST("bmp581: a power-on reset of the chip is fatal at once; recovery configures it again") {
  GuardedRig<ModelI2c> r{Standby()};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  r.bus.state.chip.PowerOnReset();
  kb::rtos::SleepFor(kb::Milliseconds{3U}); // the chip answers again after its reset time
  const kb::Result<void> res = baro::CheckForReset(r.io, kWait);
  KB_CHECK(res.Error() == kb::Status::NotReady);
  KB_CHECK_EQ(res.Detail(), kb::handlers::DetailOf(bmp581::Error::DeviceReset));
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Faulted);
  KB_CHECK(r.io.RequestRecovery(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  KB_CHECK(r.io.Sample(kWait).IsOk());
}

KB_TEST("bmp581: the SPI transport starts, samples and reads through the same handler") {
  GuardedRig<ModelSpi> r{Standby()};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  KB_CHECK(r.bus.state.chip.spi_selected);
  r.bus.state.chip.pressure_raw = 90'000 * 64;
  const auto snap = r.io.Sample(kWait);
  KB_REQUIRE(snap.IsOk());
  KB_CHECK(snap.Value().value.pressure_pa == 90000.0F);
  KB_CHECK(baro::CheckForReset(r.io, kWait).IsOk());
  r.bus.state.fail_transfer = r.bus.state.transfers;
  KB_CHECK(baro::MeasureOnce(r.io, kWait).Error() == kb::Status::BusError);
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Degraded);
}

KB_TEST("bmp581: owner model: the owner task starts, samples at its period and serves requests") {
  using Domain = kb::handlers::BusDomain<ModelSpi, kb::handlers::ThreadModel::Owner>;
  using Io = kb::handlers::OwnerAccess<baro::Handler<ModelSpi>, Domain>;
  kb_it::Timing t{};
  ModelSpi bus{};
  Domain domain{bus};
  baro::Config cfg = Standby();
  cfg.chip.mode = bmp581::PowerMode::Continuous;
  cfg.handler.sample_period = kb::Milliseconds{5U};
  Io io{domain, bus, t.timing, cfg};
  KB_REQUIRE(io.Create().IsOk());
  const kb_it::OwnerLoop<Domain, Io> owner{domain, io};
  KB_REQUIRE(io.EnsureStarted(kWait).IsOk());
  KB_CHECK(SamplesPublished(io, 1U, kWait)); // the owner samples on its own
  const auto latest = io.Latest();
  KB_REQUIRE(latest.IsOk());
  KB_CHECK(!latest.Value().value.forced);
  KB_CHECK_EQ(latest.Value().value.pressure_raw, 100'000 * 64);
  KB_CHECK(baro::CheckForReset(io, kWait).IsOk());
  KB_CHECK(baro::SetPowerMode(io, bmp581::PowerMode::Standby, kWait).IsOk());
  KB_CHECK(baro::MeasureOnce(io, kWait).IsOk());
  KB_CHECK(io.Liveness().Beats() > 0U);
  KB_CHECK(io.Stop(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(io) == HealthState::Unknown);
}

} // namespace
