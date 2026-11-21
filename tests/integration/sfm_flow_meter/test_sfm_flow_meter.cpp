// sfm_flow_meter through kb::handlers::sfm_flow_meter: the polled start-up, the snapshot, the app
// API, error mapping to the health record, recovery, and both thread models, over a
// kb::iface::I2cBus backed by the driver's own chip model
// (components/drivers/driver-sfm-flow-meter/tests/model_sfm.hpp), whose clock follows the RTOS
// clock. SPDX-License-Identifier: MIT Copyright (c) 2026 The kb-core authors
#include <cstdint>
#include <span>

#include "binding_support.hpp"
#include "datasheet_expect.hpp"
#include "kb/handlers/bindings/sfm_flow_meter.hpp"
#include "kb_test.hpp"
#include "model_sfm.hpp"

namespace {

using kb::handlers::HealthState;
namespace sfmh = kb::handlers::sfm_flow_meter;

constexpr sfm::Variant kPart = sfm::Variant::SFM3003_300_CL;

/// kb::iface::I2cBus over the driver's chip model; the model's microsecond clock follows
/// kb::rtos::Now().
class ModelBus {
public:
  model::Chip chip{*expect::Find(kPart)};
  bool up_ok{true};
  int transfers{0};

  [[nodiscard]] kb::Status EnsureReady() noexcept {
    return up_ok ? kb::Status::Ok : kb::Status::NotReady;
  }
  [[nodiscard]] kb::Status Write(uint8_t address, std::span<const uint8_t> tx) noexcept {
    sync();
    const bool general =
        (address == 0U) && (tx.size() == 1U); // the general call reaches every device
    return outcome(general ? chip.GeneralCall(tx[0]) : chip.Write(address, tx.data(), tx.size()));
  }
  [[nodiscard]] kb::Status WriteRead(uint8_t address, std::span<const uint8_t> tx,
                                     std::span<uint8_t> rx) noexcept {
    static_cast<void>(address);
    static_cast<void>(tx);
    static_cast<void>(rx);
    return kb::Status::Unsupported; // the driver never uses a repeated START
  }
  [[nodiscard]] kb::Status Read(uint8_t address, std::span<uint8_t> rx) noexcept {
    sync();
    return outcome(chip.Read(address, rx.data(), rx.size(), false));
  }
  [[nodiscard]] kb::iface::I2cFailure LastFailure() const noexcept {
    return last_;
  }

private:
  void sync() noexcept {
    ++transfers;
    chip.now_us = static_cast<uint32_t>(kb::rtos::Now().Us() - base_us_);
  }
  [[nodiscard]] kb::Status outcome(bool ok) noexcept {
    last_ = ok ? kb::iface::I2cFailure::None : kb::iface::I2cFailure::AddressNack;
    return ok ? kb::Status::Ok : kb::Status::NoDevice;
  }
  uint64_t base_us_{kb::rtos::Now().Us()};
  kb::iface::I2cFailure last_{kb::iface::I2cFailure::None};
};
static_assert(kb::iface::I2cBus<ModelBus>);

using Handler = sfmh::Handler<ModelBus>;
using GuardedBus = kb::handlers::BusDomain<ModelBus, kb::handlers::ThreadModel::Guarded>;
using OwnerBus = kb::handlers::BusDomain<ModelBus, kb::handlers::ThreadModel::Owner>;
using GuardedIo = kb::handlers::Guarded<Handler, GuardedBus>;
using OwnedIo = kb::handlers::OwnerAccess<Handler, OwnerBus>;

constexpr kb::Milliseconds kWait{500U};

sfmh::Config AirConfig() {
  sfmh::Config c{};
  c.chip = sfm::PresetFor(kPart);
  c.chip.allow_general_call_reset = true;
  c.gas = sfm::Gas::Air;
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
  explicit GuardedRig(const sfmh::Config& cfg) : io(domain, bus, t.timing, cfg) {
    static_cast<void>(domain.Create());
    static_cast<void>(io.Create());
  }
};

/// A fresh sample from the snapshot path, retried while the sensor has nothing new.
kb::Result<sfmh::Snapshot> FreshSample(GuardedIo& io) {
  kb::Result<sfmh::Snapshot> out = kb::Result<sfmh::Snapshot>::Failure(kb::Status::Timeout);
  for (int i = 0; (i < 50) && (!out.IsOk()); ++i) {
    const auto s = io.Sample(kWait);
    if (!s.IsOk()) {
      out = kb::Result<sfmh::Snapshot>::Failure(s.Error(), s.Detail());
      break;
    }
    if (s.Value().value.fresh) {
      out = kb::Result<sfmh::Snapshot>::Success(s.Value().value);
    } else {
      kb::rtos::SleepFor(kb::Milliseconds{1U});
    }
  }
  return out;
}

KB_TEST("sfm_flow_meter: guarded start runs both polled stages and the sensor measures") {
  GuardedRig r{AirConfig()};
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Unknown);
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  const kb::handlers::StartupReport rep = r.io.Startup().Value();
  KB_CHECK(rep.complete);
  KB_CHECK_EQ(rep.stages, 2U);
  KB_CHECK_EQ(rep.stage, 1U);
  KB_CHECK_EQ(rep.kinds, kb::handlers::stage::kConfigure);
  KB_CHECK(rep.stage_us[0] > 0U); // the wake and identification waits were polled
  KB_CHECK(r.bus.chip.Measuring());
  const int before = r.bus.transfers;
  KB_CHECK(r.io.EnsureStarted(kWait).IsOk()); // idempotent, no traffic
  KB_CHECK_EQ(r.bus.transfers, before);
  KB_CHECK_EQ(r.io.Health().Value().starts, 1U);
}

KB_TEST("sfm_flow_meter: without measure_on_start the Configure stage is skipped") {
  sfmh::Config cfg = AirConfig();
  cfg.measure_on_start = false;
  GuardedRig r{cfg};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  KB_CHECK(!r.bus.chip.Measuring());
  const auto s = r.io.Sample(kWait);
  KB_REQUIRE(s.IsOk());
  KB_CHECK(!s.Value().value.fresh);
  KB_CHECK(s.Value().value.state == sfm::State::Idle);
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
}

KB_TEST("sfm_flow_meter: calls before a start send nothing and return NotReady") {
  GuardedRig r{AirConfig()};
  KB_CHECK(sfmh::StopMeasurement(r.io, kWait).Error() == kb::Status::NotReady);
  KB_CHECK(sfmh::ReadSample(r.io, kWait).Error() == kb::Status::NotReady);
  KB_CHECK(r.io.Sample(kWait).Error() == kb::Status::NotReady);
  KB_CHECK_EQ(r.bus.transfers, 0);
}

KB_TEST("sfm_flow_meter: an absent sensor fails stage 0 with NoDevice; a wrong part fails with "
        "Fault; retries start again") {
  GuardedRig r{AirConfig()};
  r.bus.chip.dead = true;
  const kb::Result<void> first = r.io.EnsureStarted(kWait);
  KB_CHECK(first.Error() == kb::Status::NoDevice);
  KB_CHECK_EQ(first.Detail(), kb::handlers::DetailOf(sfm::Error::NoDevice));
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Faulted);
  KB_CHECK_EQ(r.io.Startup().Value().stage, 0U);
  r.bus.chip.dead = false;
  r.bus.chip.id_override = 0x04020700U; // another part's identifier
  const kb::Result<void> second = r.io.EnsureStarted(kWait);
  KB_CHECK(second.Error() == kb::Status::Fault);
  KB_CHECK_EQ(second.Detail(), kb::handlers::DetailOf(sfm::Error::DeviceIdMismatch));
  KB_CHECK_EQ(r.io.Health().Value().start_failures, 2U);
  r.bus.chip.id_override = 0U;
  KB_CHECK(r.io.EnsureStarted(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  KB_CHECK_EQ(r.io.Startup().Value().runs, 3U);
}

KB_TEST("sfm_flow_meter: the snapshot carries the flow, ReadSample reads now") {
  GuardedRig r{AirConfig()};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  r.bus.chip.flow_ml = 30000;
  const auto snap = FreshSample(r.io);
  KB_REQUIRE(snap.IsOk());
  KB_CHECK_EQ(snap.Value().sample.flow_ml_per_min, 30000);
  KB_CHECK(snap.Value().state == sfm::State::Measuring);
  KB_CHECK(r.io.Latest().IsOk());
  KB_CHECK(r.io.Latest().Value().value.fresh);
  r.bus.chip.flow_ml = -12000;
  kb::rtos::SleepFor(kb::Milliseconds{2U});
  kb::Result<sfm::Sample> now = sfmh::ReadSample(r.io, kWait);
  for (int i = 0; (i < 20) && (now.Error() == kb::Status::Empty); ++i) {
    kb::rtos::SleepFor(kb::Milliseconds{1U});
    now = sfmh::ReadSample(r.io, kWait);
  }
  KB_REQUIRE(now.IsOk());
  KB_CHECK_EQ(now.Value().flow_ml_per_min, -12000);
}

KB_TEST(
    "sfm_flow_meter: stop, averaging, sleep, wake, start and a soft reset through the app API") {
  GuardedRig r{AirConfig()};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  KB_CHECK(sfmh::StopMeasurement(r.io, kWait).IsOk());
  KB_CHECK(!r.bus.chip.Measuring());
  KB_CHECK(sfmh::SetAveraging(r.io, 16U, kWait).IsOk());
  KB_CHECK_EQ(r.bus.chip.Averaging(), 16U);
  KB_CHECK(sfmh::SetSleep(r.io, true, kWait).IsOk());
  KB_CHECK(r.bus.chip.Asleep());
  KB_CHECK(sfmh::SetSleep(r.io, false, kWait).IsOk());
  KB_CHECK(!r.bus.chip.Asleep());
  KB_CHECK(sfmh::StartMeasurement(r.io, sfm::Gas::O2, kWait).IsOk());
  KB_CHECK(r.bus.chip.Measuring());
  sfmh::Command reset{};
  reset.op = sfmh::Op::SoftReset;
  const auto rr = r.io.Request(reset, kWait);
  KB_REQUIRE(rr.IsOk());
  KB_CHECK(rr.Value().state ==
           sfm::State::Measuring); // identified again, the configured gas restarted
  KB_CHECK_EQ(r.bus.chip.resets, 1U);
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
}

KB_TEST("sfm_flow_meter: CRC failures degrade, the threshold faults, Recover() restores Ready") {
  GuardedRig r{AirConfig()};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  KB_REQUIRE(FreshSample(r.io).IsOk());
  KB_CHECK(sfmh::ReadTemperature(r.io, kWait).Error() ==
           kb::Status::Unsupported); // SF06: in the frame only
  KB_CHECK_EQ(r.io.Health().Value().last_detail, kb::handlers::DetailOf(sfm::Error::Unsupported));
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Degraded);
  KB_REQUIRE(FreshSample(r.io).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  r.bus.chip.flip_read_byte = 1; // every frame fails its CRC
  const auto bad = FreshSample(r.io);
  KB_CHECK(bad.Error() == kb::Status::CrcMismatch);
  KB_CHECK_EQ(bad.Detail(), kb::handlers::DetailOf(sfm::Error::Crc));
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Degraded);
  KB_CHECK(!FreshSample(r.io).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Faulted);
  const int before = r.bus.transfers;
  KB_CHECK(r.io.Sample(kWait).Error() == kb::Status::NotReady);
  KB_CHECK_EQ(r.bus.transfers, before); // reads never restart a faulted sensor
  r.bus.chip.flip_read_byte = -1;
  KB_CHECK(r.io.RequestRecovery(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  KB_CHECK(r.bus.chip.Measuring());
  KB_CHECK_EQ(r.io.Health().Value().recoveries, 1U);
}

KB_TEST("sfm_flow_meter: a sensor that left its measurement is fatal at once (DeviceReset)") {
  GuardedRig r{AirConfig()};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  KB_REQUIRE(FreshSample(r.io).IsOk());
  r.bus.chip.BrownOut(); // the sensor resets by itself: idle, silent
  kb::Result<sfmh::Snapshot> s = kb::Result<sfmh::Snapshot>::Success(sfmh::Snapshot{});
  for (int i = 0; (i < 100) && s.IsOk(); ++i) {
    kb::rtos::SleepFor(kb::Milliseconds{1U});
    const auto one = r.io.Sample(kWait);
    s = one.IsOk() ? kb::Result<sfmh::Snapshot>::Success(one.Value().value)
                   : kb::Result<sfmh::Snapshot>::Failure(one.Error(), one.Detail());
  }
  KB_CHECK(s.Error() == kb::Status::NotReady);
  KB_CHECK_EQ(s.Detail(), kb::handlers::DetailOf(sfm::Error::DeviceReset));
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Faulted);
  KB_CHECK(r.io.RequestRecovery(kWait).IsOk());
  KB_CHECK(r.bus.chip.Measuring()); // the plan started the configured gas again
}

KB_TEST(
    "sfm_flow_meter: owner model: the owner task polls the start, samples and serves requests") {
  kb_it::Timing t{};
  ModelBus bus{};
  OwnerBus domain{bus};
  sfmh::Config cfg = AirConfig();
  cfg.handler.sample_period = kb::Milliseconds{2U};
  OwnedIo io{domain, bus, t.timing, cfg};
  KB_REQUIRE(io.Create().IsOk());
  const kb_it::OwnerLoop<OwnerBus, OwnedIo> owner{domain, io};
  bus.chip.flow_ml = 45000;
  KB_REQUIRE(io.EnsureStarted(kWait).IsOk());
  KB_CHECK(bus.chip.Measuring());
  bool fresh = false;
  for (int i = 0; (i < 100) && (!fresh); ++i) {
    kb::rtos::SleepFor(kb::Milliseconds{2U});
    const auto latest = io.Latest();
    fresh = latest.IsOk() && latest.Value().value.fresh &&
            (latest.Value().value.sample.flow_ml_per_min == 45000);
  }
  KB_CHECK(fresh);
  KB_CHECK(sfmh::StopMeasurement(io, kWait).IsOk());
  KB_CHECK(!bus.chip.Measuring());
  KB_CHECK(sfmh::StartMeasurement(io, sfm::Gas::Air, kWait).IsOk());
  KB_CHECK(io.Liveness().Beats() > 0U);
  KB_CHECK(io.Stop(kWait).IsOk());
  KB_CHECK(!bus.chip.Measuring()); // Stop() stops the measurement before forgetting the sensor
  KB_CHECK(kb_it::StateOf(io) == HealthState::Unknown);
}

} // namespace
