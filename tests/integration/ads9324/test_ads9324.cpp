// ads9324 through kb::handlers::ads9324: start-up, the app API, error mapping to the health record,
// recovery, and both thread models, over a kb::iface::SpiDevice and kb::iface GPIO pins backed by
// the driver's own chip model (components/drivers/driver-ads9324/tests/mock_ads9324.hpp).
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include <cstdint>
#include <span>

#include "binding_support.hpp"
#include "kb/handlers/bindings/ads9324.hpp"
#include "kb_test.hpp"
#include "mock_ads9324.hpp"

namespace {

using kb::handlers::HealthState;
using kb::iface::Level;
namespace adc = kb::handlers::ads9324;

/// kb::iface::SpiDevice over the driver's chip model. The model's clock follows the RTOS clock, so
/// the driver's waits (30 ms power-up, conversion time) pass in the model as they pass on the host.
class ModelDev {
public:
  ads9324_test::Ads9324Model chip{};
  bool up_ok{true};

  [[nodiscard]] kb::Status EnsureReady() noexcept {
    return up_ok ? kb::Status::Ok : kb::Status::NotReady;
  }
  [[nodiscard]] kb::Status Transfer(std::span<const uint8_t> tx, std::span<uint8_t> rx) noexcept {
    Sync();
    const std::size_t len = (tx.size() > rx.size()) ? tx.size() : rx.size();
    return chip.Transfer(tx.empty() ? nullptr : tx.data(), rx.empty() ? nullptr : rx.data(), len)
               ? kb::Status::Ok
               : kb::Status::BusError;
  }
  void Sync() noexcept {
    const uint64_t now = kb::rtos::Now().Us();
    chip.Delay(static_cast<uint32_t>(now - last_us_));
    last_us_ = now;
  }

private:
  uint64_t last_us_{kb::rtos::Now().Us()}; // the model's clock starts with the object
};
static_assert(kb::iface::SpiDevice<ModelDev>);

/// The RESET pin of the model (electrically active low).
class ResetPin {
public:
  explicit ResetPin(ModelDev& dev) noexcept : dev_(dev) {}
  [[nodiscard]] kb::Status Set(Level level) noexcept {
    dev_.Sync();
    dev_.chip.SetResetPin(level == Level::Low);
    level_ = level;
    return kb::Status::Ok;
  }
  [[nodiscard]] kb::Status Toggle() noexcept {
    return Set((level_ == Level::High) ? Level::Low : Level::High);
  }

private:
  ModelDev& dev_;
  Level level_{Level::High};
};

/// The CONVST pin of the model (active high).
class ConvstPin {
public:
  explicit ConvstPin(ModelDev& dev) noexcept : dev_(dev) {}
  [[nodiscard]] kb::Status Set(Level level) noexcept {
    dev_.Sync();
    dev_.chip.SetConvst(level == Level::High);
    level_ = level;
    return kb::Status::Ok;
  }
  [[nodiscard]] kb::Status Toggle() noexcept {
    return Set((level_ == Level::High) ? Level::Low : Level::High);
  }

private:
  ModelDev& dev_;
  Level level_{Level::Low};
};

/// The DRDY/ALARM pin of the model (its electrical level).
class AlarmPin {
public:
  explicit AlarmPin(ModelDev& dev) noexcept : dev_(dev) {}
  [[nodiscard]] kb::Status Get(Level& out) noexcept {
    dev_.Sync();
    out = dev_.chip.AlarmPin() ? Level::High : Level::Low;
    return kb::Status::Ok;
  }

private:
  ModelDev& dev_;
};
static_assert(kb::iface::GpioOutput<ResetPin> && kb::iface::GpioInput<AlarmPin>);

using Handler = adc::Handler<ModelDev>;
using GuardedBus = kb::handlers::BusDomain<ModelDev, kb::handlers::ThreadModel::Guarded>;
using OwnerBus = kb::handlers::BusDomain<ModelDev, kb::handlers::ThreadModel::Owner>;
using GuardedIo = kb::handlers::Guarded<Handler, GuardedBus>;
using OwnedIo = kb::handlers::OwnerAccess<Handler, OwnerBus>;

constexpr kb::Milliseconds kWait{1000U};

/// The board: the model and its three control pins.
struct Board {
  ModelDev dev{};
  ResetPin reset{dev};
  ConvstPin convst{dev};
  AlarmPin alarm{dev};

  Board() {
    dev.chip.analog_offset_lsb = 0;
    for (std::size_t ch = 0U; ch < 16U; ++ch) {
      dev.chip.input_uv[ch] = -4'000'000 + (500'000 * static_cast<int32_t>(ch));
    }
  }

  adc::Config Wired() {
    adc::Config c{};
    c.chip.interface.append_diag = true; // the DIAG byte shows a reset
    c.outputs[adc::kPinReset].pin.emplace(reset);
    c.outputs[adc::kPinReset].active_low = true;
    c.outputs[adc::kPinConvst].pin.emplace(convst);
    c.inputs[adc::kPinAlarm].pin.emplace(alarm);
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
  explicit GuardedRig(bool hardware_reset = false)
      : io(domain, board.dev, t.timing, config(hardware_reset)) {
    static_cast<void>(domain.Create());
    static_cast<void>(io.Create());
  }
  adc::Config config(bool hardware_reset) {
    adc::Config c = board.Wired();
    c.chip.hardware_reset = hardware_reset;
    return c;
  }
  ads9324_test::Ads9324Model& Chip() {
    return board.dev.chip;
  }
};

bool Near(int32_t got, int32_t want) {
  return ((got - want) < 400) && ((want - got) < 400);
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

KB_TEST("ads9324: guarded start runs the one sync stage and the handler is Ready") {
  GuardedRig r{};
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Unknown);
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  const kb::handlers::StartupReport rep = r.io.Startup().Value();
  KB_CHECK(rep.complete);
  KB_CHECK_EQ(rep.stages, 1U);
  KB_CHECK_EQ(rep.kinds, kb::handlers::stage::kIdentify | kb::handlers::stage::kConfigure);
  KB_CHECK(r.io.EnsureStarted(kWait).IsOk()); // idempotent
  KB_CHECK_EQ(r.io.Health().Value().starts, 1U);
  const kb::Result<uint16_t> id = adc::Identify(r.io, kWait);
  KB_REQUIRE(id.IsOk());
  KB_CHECK_EQ(id.Value(), ads9324::kDeviceId);
}

KB_TEST("ads9324: calls before a start send nothing and return NotReady") {
  GuardedRig r{};
  const int before = r.Chip().transfers;
  KB_CHECK(adc::Identify(r.io, kWait).Error() == kb::Status::NotReady);
  KB_CHECK(r.io.Sample(kWait).Error() == kb::Status::NotReady);
  KB_CHECK_EQ(r.Chip().transfers, before);
}

KB_TEST("ads9324: a wrong DEVICE_ID fails the start with Fault and the driver's code; a retry "
        "starts again") {
  GuardedRig r{};
  r.Chip().knobs.device_id = 5U;
  const kb::Result<void> first = r.io.EnsureStarted(kWait);
  KB_CHECK(first.Error() == kb::Status::Fault);
  KB_CHECK_EQ(first.Detail(), kb::handlers::DetailOf(ads9324::Error::DeviceIdMismatch));
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Faulted);
  KB_CHECK_EQ(r.io.Health().Value().start_failures, 1U);
  KB_CHECK(r.io.Startup().Value().status == kb::Status::Fault);
  r.Chip().knobs.device_id = 4U;
  KB_CHECK(r.io.EnsureStarted(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
}

KB_TEST("ads9324: a hardware reset through the RESET pin starts the chip") {
  GuardedRig r{true};
  const uint32_t resets = r.Chip().resets;
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  KB_CHECK(r.Chip().resets > resets);
  adc::Command c{};
  c.op = adc::Op::Reset;
  c.reset = ads9324::ResetKind::Hardware;
  KB_CHECK(r.io.Request(c, kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
}

KB_TEST(
    "ads9324: a sample converts all 16 channels; window flags, alarm pin, self-test and standby") {
  GuardedRig r{};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  const auto snap = r.io.Sample(kWait);
  KB_REQUIRE(snap.IsOk());
  KB_CHECK_EQ(snap.Value().value.present_mask, 0xFFFFU);
  KB_CHECK(Near(snap.Value().value.microvolts[0], -4'000'000));
  KB_CHECK(Near(snap.Value().value.microvolts[15], 3'500'000));
  KB_CHECK(!snap.Value().value.standby);
  KB_CHECK(adc::ReadWindowFlags(r.io, kWait).IsOk());
  KB_CHECK(adc::ClearWindowFlags(r.io, 1U, kWait).IsOk());
  KB_CHECK(adc::ClearWindowFlags(r.io, 2U, kWait).Error() == kb::Status::InvalidArgument);
  adc::Command pin{};
  pin.op = adc::Op::ReadAlarmPin;
  KB_CHECK(r.io.Request(pin, kWait).IsOk());
  KB_CHECK(adc::RunSelfTest(r.io, kWait).IsOk());
  KB_CHECK(adc::SetStandby(r.io, true, kWait).IsOk());
  const int before = r.Chip().transfers;
  const auto parked = r.io.Sample(kWait);
  KB_REQUIRE(parked.IsOk());
  KB_CHECK(parked.Value().value.standby);
  KB_CHECK(Near(parked.Value().value.microvolts[0], -4'000'000)); // the last frame
  KB_CHECK_EQ(r.Chip().transfers, before);                        // nothing sent in standby
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  KB_CHECK(adc::SetStandby(r.io, false, kWait).IsOk());
  KB_CHECK(!r.io.Sample(kWait).Value().value.standby);
}

KB_TEST("ads9324: stuck data degrades, the threshold faults, Recover() restores Ready") {
  GuardedRig r{};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  r.Chip().knobs.fail_transfer = r.Chip().transfers; // the next transfer fails on the bus
  const auto bad = r.io.Sample(kWait);
  KB_CHECK(bad.Error() == kb::Status::BusError);
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Degraded);
  KB_CHECK(r.io.Sample(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  r.Chip().knobs.float_sdout = true;
  const auto stuck = r.io.Sample(kWait);
  KB_CHECK(stuck.Error() == kb::Status::BusError);
  KB_CHECK_EQ(stuck.Detail(), kb::handlers::DetailOf(ads9324::Error::DataMismatch));
  KB_CHECK(!r.io.Sample(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Faulted);
  const int before = r.Chip().transfers;
  KB_CHECK(adc::Identify(r.io, kWait).Error() == kb::Status::NotReady);
  KB_CHECK_EQ(r.Chip().transfers, before); // nothing restarts a faulted chip
  r.Chip().knobs.float_sdout = false;
  KB_CHECK(r.io.RequestRecovery(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  KB_CHECK_EQ(r.io.Health().Value().recoveries, 1U);
}

KB_TEST("ads9324: a reset seen in DIAG is fatal at once; recovery configures the chip again") {
  GuardedRig r{};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  r.Chip().knobs.reset_during_run = true;
  const auto res = r.io.Sample(kWait);
  KB_CHECK(res.Error() == kb::Status::NotReady);
  KB_CHECK_EQ(res.Detail(), kb::handlers::DetailOf(ads9324::Error::UnexpectedReset));
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Faulted);
  r.Chip().knobs.reset_during_run = false;
  KB_CHECK(r.io.RequestRecovery(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  KB_CHECK(r.io.Sample(kWait).IsOk());
}

KB_TEST("ads9324: owner model: the owner task starts, samples at its period and serves requests") {
  kb_it::Timing t{};
  Board board{};
  OwnerBus domain{board.dev};
  adc::Config cfg = board.Wired();
  cfg.handler.sample_period = kb::Milliseconds{2U};
  OwnedIo io{domain, board.dev, t.timing, cfg};
  KB_REQUIRE(io.Create().IsOk());
  const kb_it::OwnerLoop<OwnerBus, OwnedIo> owner{domain, io};
  KB_REQUIRE(io.EnsureStarted(kWait).IsOk());
  KB_CHECK(SamplesPublished(io, 1U, kWait)); // the owner samples on its own
  const auto latest = io.Latest();
  KB_REQUIRE(latest.IsOk());
  KB_CHECK(Near(latest.Value().value.microvolts[4], -2'000'000));
  KB_CHECK_EQ(adc::Identify(io, kWait).Value(), ads9324::kDeviceId);
  KB_CHECK(adc::ReadWindowFlags(io, kWait).IsOk());
  KB_CHECK(io.Liveness().Beats() > 0U);
  KB_CHECK(io.Stop(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(io) == HealthState::Unknown);
}

} // namespace
