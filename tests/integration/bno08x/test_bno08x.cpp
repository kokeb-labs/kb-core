// bno08x through kb::handlers::bno08x: the owner-only start-up plan (Begin, Poll until Ready, one
// SetFeature per sensor), samples and operation records in the snapshot, the app API, hub restarts
// (Degraded) and a faulted session (Faulted, then the plan again), over a kb::iface::I2cBus and
// kb::iface GPIO pins backed by the driver's own hub model
// (components/drivers/driver-bno08x/tests/model/bno08x_hub_model.hpp). Owner model only:
// Guarded<Handler> does not compile for this chip (kRequiresOwner). SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

#include "binding_support.hpp"
#include "bno08x_hub_model.hpp"
#include "kb/handlers/bindings/bno08x.hpp"
#include "kb_test.hpp"

namespace {

using kb::handlers::HealthState;
namespace hb = kb::handlers::bno08x;

/// The hub model on its own clock (microseconds since the rig was built). Only the owner task
/// touches the model once the owner loop runs; the test thread changes it through the atomics,
/// applied at the next tick.
struct HubRig {
  hubmodel::Hub hub{};
  uint32_t origin{kb::utils::NarrowUs(kb::rtos::Now())};
  std::atomic<bool> omit_init{false}; ///< The hub never sends its initialise response.
  std::atomic<uint32_t> transfers{0U};
  HubRig() {
    hub.PowerOn(Now());
  }
  [[nodiscard]] uint32_t Now() const {
    return kb::utils::NarrowUs(kb::rtos::Now()) - origin;
  }
  void Tick() {
    hub.k.omit_init_response = omit_init.load();
    hub.Tick(Now());
  }
};

/// kb::iface::I2cBus over the hub model's I2C front.
class ModelI2c {
public:
  explicit ModelI2c(HubRig& r) : r_(r) {}
  [[nodiscard]] kb::Status EnsureReady() noexcept {
    return kb::Status::Ok;
  }
  [[nodiscard]] kb::Status Write(uint8_t address, std::span<const uint8_t> tx) noexcept {
    static_cast<void>(address);
    r_.Tick();
    r_.transfers.fetch_add(1U);
    return r_.hub.HostWrite(tx.data(), tx.size()) ? kb::Status::Ok : kb::Status::Nack;
  }
  [[nodiscard]] kb::Status WriteRead(uint8_t address, std::span<const uint8_t> tx,
                                     std::span<uint8_t> rx) noexcept {
    static_cast<void>(address);
    static_cast<void>(tx);
    static_cast<void>(rx);
    return kb::Status::Unsupported; // SHTP never uses a repeated start
  }
  [[nodiscard]] kb::Status Read(uint8_t address, std::span<uint8_t> rx) noexcept {
    static_cast<void>(address);
    r_.Tick();
    r_.transfers.fetch_add(1U);
    return r_.hub.HostRead(rx.data(), rx.size()) ? kb::Status::Ok : kb::Status::Nack;
  }
  [[nodiscard]] kb::iface::I2cFailure LastFailure() const noexcept {
    return kb::iface::I2cFailure::None;
  }

private:
  HubRig& r_;
};
static_assert(kb::iface::I2cBus<ModelI2c>);

/// H_INTN of the model (low while the hub has data).
class ModelInt {
public:
  explicit ModelInt(HubRig& r) : r_(r) {}
  [[nodiscard]] kb::Status Get(kb::iface::Level& out) noexcept {
    r_.Tick();
    out = r_.hub.IntActive() ? kb::iface::Level::Low : kb::iface::Level::High;
    return kb::Status::Ok;
  }

private:
  HubRig& r_;
};

/// NRST of the model.
class ModelNrst {
public:
  explicit ModelNrst(HubRig& r) : r_(r) {}
  [[nodiscard]] kb::Status Set(kb::iface::Level level) noexcept {
    r_.hub.Nrst(level == kb::iface::Level::High, r_.Now());
    return kb::Status::Ok;
  }
  [[nodiscard]] kb::Status Toggle() noexcept {
    return kb::Status::Unsupported;
  }

private:
  HubRig& r_;
};

using Handler = hb::Handler<ModelI2c>;
using JoinHandler = hb::Handler<ModelI2c, false>;
using OwnerBus = kb::handlers::BusDomain<ModelI2c, kb::handlers::ThreadModel::Owner>;
using Imu = kb::handlers::OwnerAccess<Handler, OwnerBus>;
using JoinImu = kb::handlers::OwnerAccess<JoinHandler, OwnerBus>;

static_assert(Handler::kRequiresOwner);
static_assert(Handler::kBus == kb::handlers::BusKind::I2c);

constexpr kb::Milliseconds kStart{2000U};
constexpr kb::Milliseconds kWait{500U};

::bno08x::FeatureConfig Every(uint32_t interval_us) {
  ::bno08x::FeatureConfig f{};
  f.interval_us = interval_us;
  return f;
}

/// Accelerometer and rotation vector at 100 Hz; short driver waits.
hb::Config Imu2() {
  hb::Config c{};
  c.chip.startup_timeout_ms = 300U;
  c.chip.op_timeout_ms = 300U;
  c.sensors[0] = hb::SensorSetting{Every(10'000U), ::bno08x::SensorId::Accelerometer};
  c.sensors[1] = hb::SensorSetting{Every(10'000U), ::bno08x::SensorId::RotationVector};
  c.sensor_count = 2U;
  c.handler.health.fault_after = 2U;
  c.handler.recovery.backoff = kb::Milliseconds{1U};
  return c;
}

/// The hub, its bus and pins, the owner domain and the access (created; the owner loop is the
/// test's).
template <typename Access>
struct Rig {
  kb_it::Timing t{};
  kb::handlers::HookTiming timing{t.timing};
  HubRig hub{};
  ModelI2c bus{hub};
  ModelInt int_pin{hub};
  ModelNrst nrst{hub};
  OwnerBus domain{bus};
  Access io;
  explicit Rig(hb::Config cfg) : io(domain, bus, timing, Pins(cfg)) {
    static_cast<void>(io.Create());
  }
  hb::Config Pins(hb::Config cfg) {
    cfg.pins.inputs[hb::kPinInt] = kb::handlers::InputPin{kb::iface::GpioInputRef{int_pin}, true};
    cfg.pins.outputs[hb::kPinNrst] = kb::handlers::OutputPin{kb::iface::GpioOutputRef{nrst}, true};
    return cfg;
  }
};

/// Waits until `pred` holds or `ms` pass.
template <typename Pred>
bool Eventually(Pred pred, uint32_t ms) {
  const kb::utils::Deadline end = kb::utils::Deadline::After(kb::rtos::Now(), kb::Milliseconds{ms});
  bool ok = pred();
  while ((!ok) && (!end.Expired(kb::rtos::Now()))) {
    kb::rtos::SleepFor(kb::Milliseconds{2U});
    ok = pred();
  }
  return ok;
}

template <typename Access>
uint32_t Count(const Access& io, ::bno08x::SensorId id) {
  const kb::Result<hb::Sample> s = hb::LatestSample(io, id);
  return s.IsOk() ? s.Value().count : 0U;
}

KB_TEST("bno08x: the owner runs the three-stage plan and the configured sensors stream into the "
        "snapshot") {
  Rig<Imu> r{Imu2()};
  const kb_it::OwnerLoop<OwnerBus, Imu> owner{r.domain, r.io};
  KB_REQUIRE(r.io.EnsureStarted(kStart).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  const kb::handlers::StartupReport rep = r.io.Startup().Value();
  KB_CHECK(rep.complete);
  KB_CHECK_EQ(rep.stages, 3U);
  KB_CHECK_EQ(rep.stage, 2U);
  KB_CHECK_EQ(rep.kinds, kb::handlers::stage::kConfigure);
  KB_CHECK(rep.stage_us[1] > 50'000U); // the reset and the boot take their time
  KB_CHECK(Eventually([&] { return Count(r.io, ::bno08x::SensorId::Accelerometer) > 3U; }, 500U));
  KB_CHECK(Eventually([&] { return Count(r.io, ::bno08x::SensorId::RotationVector) > 3U; }, 500U));
  const auto snap = r.io.Latest();
  KB_REQUIRE(snap.IsOk());
  KB_CHECK(snap.Value().value.state == ::bno08x::State::Ready);
  KB_CHECK_EQ(r.io.Health().Value().starts, 1U);
}

KB_TEST("bno08x: calls before a start send nothing and return NotReady") {
  Rig<Imu> r{Imu2()};
  const kb_it::OwnerLoop<OwnerBus, Imu> owner{r.domain, r.io};
  const uint32_t before = r.hub.transfers.load();
  KB_CHECK(hb::ReadProductId(r.io, kWait).Error() == kb::Status::NotReady);
  KB_CHECK(r.io.Sample(kWait).Error() == kb::Status::NotReady);
  KB_CHECK(r.io.Latest().Error() == kb::Status::NotReady);
  KB_CHECK_EQ(r.hub.transfers.load(), before);
}

KB_TEST("bno08x: a hub that never completes its start fails the session stage with Timeout; a "
        "retry starts again") {
  Rig<Imu> r{Imu2()};
  r.hub.omit_init = true;
  const kb_it::OwnerLoop<OwnerBus, Imu> owner{r.domain, r.io};
  const kb::Result<void> first = r.io.EnsureStarted(kStart);
  KB_CHECK(first.Error() == kb::Status::Timeout);
  KB_CHECK_EQ(first.Detail(), kb::handlers::DetailOf(::bno08x::Error::Timeout));
  const kb::handlers::StartupReport rep = r.io.Startup().Value();
  KB_CHECK(!rep.complete);
  KB_CHECK_EQ(rep.stage, hb::kStageSession);
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Faulted);
  r.hub.omit_init = false;
  KB_CHECK(r.io.EnsureStarted(kStart).IsOk());
  KB_CHECK_EQ(r.io.Startup().Value().runs, 2U);
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
}

KB_TEST("bno08x: a sensor the chip lacks fails the configure stage with the driver's code") {
  hb::Config cfg = Imu2();
  cfg.sensors[1] =
      hb::SensorSetting{Every(10'000U), ::bno08x::SensorId::MotionRequest}; // BNO086 only
  Rig<Imu> r{cfg};
  const kb_it::OwnerLoop<OwnerBus, Imu> owner{r.domain, r.io};
  const kb::Result<void> res = r.io.EnsureStarted(kStart);
  KB_CHECK(res.Error() == kb::Status::Unsupported);
  KB_CHECK_EQ(res.Detail(), kb::handlers::DetailOf(::bno08x::Error::Variant));
  KB_CHECK_EQ(r.io.Startup().Value().stage, hb::kStageConfigure);
}

KB_TEST("bno08x: set sensor, product ID and tare complete as operation records") {
  Rig<Imu> r{Imu2()};
  const kb_it::OwnerLoop<OwnerBus, Imu> owner{r.domain, r.io};
  KB_REQUIRE(r.io.EnsureStarted(kStart).IsOk());
  const kb::Result<::bno08x::OpId> grav =
      hb::SetSensor(r.io, ::bno08x::SensorId::Gravity, Every(20'000U), kWait);
  KB_REQUIRE(grav.IsOk());
  KB_CHECK(hb::WaitOp(r.io, grav.Value(), kWait).IsOk());
  KB_CHECK(Eventually([&] { return Count(r.io, ::bno08x::SensorId::Gravity) > 2U; }, 500U));
  const kb::Result<::bno08x::OpId> pid = hb::ReadProductId(r.io, kWait);
  KB_REQUIRE(pid.IsOk());
  KB_CHECK(hb::WaitOp(r.io, pid.Value(), kWait).IsOk());
  KB_CHECK(Eventually(
      [&] { return r.io.Latest().IsOk() && r.io.Latest().Value().value.product.valid; }, 200U));
  const kb::Result<::bno08x::OpId> tare =
      hb::Tare(r.io, ::bno08x::AxisMask::All, ::bno08x::TareBasis::RotationVector, kWait);
  KB_REQUIRE(tare.IsOk());
  KB_CHECK(hb::WaitOp(r.io, tare.Value(), kWait).IsOk());
  hb::Command flush{};
  flush.op = hb::Op::Flush;
  flush.sensor = ::bno08x::SensorId::Accelerometer;
  const kb::Result<::bno08x::OpId> fl = hb::Submit(r.io, flush, kWait);
  KB_REQUIRE(fl.IsOk());
  KB_CHECK(hb::WaitOp(r.io, fl.Value(), kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
}

KB_TEST("bno08x: unsupported calls degrade, the threshold faults, RequestRecovery runs the plan "
        "again") {
  hb::Config cfg = Imu2();
  cfg.handler.sample_period =
      kb::Milliseconds{5000U}; // no good sample between the two refused calls
  Rig<Imu> r{cfg};
  const kb_it::OwnerLoop<OwnerBus, Imu> owner{r.domain, r.io};
  KB_REQUIRE(r.io.EnsureStarted(kStart).IsOk());
  KB_REQUIRE(
      Eventually([&] { return r.io.Latest().IsOk(); }, 200U)); // the first sample is behind us
  const kb::Result<::bno08x::OpId> bad = hb::Tare(
      r.io, ::bno08x::AxisMask::X, ::bno08x::TareBasis::RotationVector, kWait); // refused mask
  KB_CHECK(bad.Error() == kb::Status::InvalidArgument);
  KB_CHECK_EQ(bad.Detail(), kb::handlers::DetailOf(::bno08x::Error::InvalidArgument));
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready); // a caller's mistake is not counted
  KB_CHECK_EQ(r.io.Health().Value().refused_calls, 1U);
  hb::Command off{};
  off.op = hb::Op::SetSensor;
  off.sensor = ::bno08x::SensorId::WheelEncoder; // no documented layout
  off.feature = Every(10'000U);
  const kb::Result<::bno08x::OpId> wheel = hb::Submit(r.io, off, kWait);
  KB_CHECK(wheel.Error() == kb::Status::Unsupported);
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Degraded);
  KB_CHECK(hb::Submit(r.io, off, kWait).Error() == kb::Status::Unsupported);
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Faulted);
  KB_CHECK(hb::ReadProductId(r.io, kWait).Error() == kb::Status::NotReady);
  KB_CHECK(r.io.RequestRecovery(kStart).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  KB_CHECK_EQ(r.io.Health().Value().recoveries, 1U);
}

KB_TEST("bno08x: a hub restart is Degraded, not Faulted, and the driver restores the sensors") {
  Rig<Imu> r{Imu2()};
  const kb_it::OwnerLoop<OwnerBus, Imu> owner{r.domain, r.io};
  KB_REQUIRE(r.io.EnsureStarted(kStart).IsOk());
  KB_CHECK(Eventually([&] { return Count(r.io, ::bno08x::SensorId::Accelerometer) > 2U; }, 500U));
  KB_REQUIRE(hb::ResetHub(r.io, kWait).IsOk());
  KB_CHECK(Eventually(
      [&] { return r.io.Latest().IsOk() && (r.io.Latest().Value().value.restarts == 1U); }, 1000U));
  const kb::handlers::HealthRecord h = r.io.Health().Value();
  KB_CHECK_EQ(h.last_detail, kb::handlers::DetailOf(::bno08x::Error::HubReset));
  KB_CHECK(h.failed_calls >= 1U);
  KB_CHECK_EQ(h.starts, 1U); // no new start: the driver restores
  const uint32_t after = Count(r.io, ::bno08x::SensorId::Accelerometer);
  KB_CHECK(Eventually([&] { return Count(r.io, ::bno08x::SensorId::Accelerometer) > (after + 3U); },
                      1000U));
  KB_CHECK(Eventually([&] { return kb_it::StateOf(r.io) == HealthState::Ready; }, 200U));
}

KB_TEST("bno08x: a restart that never completes faults the session at once; the recovery runs the "
        "plan") {
  Rig<Imu> r{Imu2()};
  const kb_it::OwnerLoop<OwnerBus, Imu> owner{r.domain, r.io};
  KB_REQUIRE(r.io.EnsureStarted(kStart).IsOk());
  r.hub.omit_init = true;
  KB_REQUIRE(hb::ResetHub(r.io, kWait).IsOk());
  KB_CHECK(Eventually([&] { return kb_it::StateOf(r.io) == HealthState::Faulted; }, 1500U));
  KB_CHECK_EQ(r.io.Health().Value().last_detail,
              kb::handlers::DetailOf(::bno08x::Error::NotInitialized));
  KB_CHECK(hb::ReadProductId(r.io, kWait).Error() == kb::Status::NotReady);
  r.hub.omit_init = false;
  KB_CHECK(r.io.RequestRecovery(kStart).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  KB_CHECK_EQ(r.io.Health().Value().recoveries, 1U);
  KB_CHECK(Eventually([&] { return Count(r.io, ::bno08x::SensorId::RotationVector) > 2U; }, 500U));
}

KB_TEST("bno08x: without NRST the plan joins the running hub (Link, Identify, Configure)") {
  Rig<JoinImu> r{Imu2()};
  kb::rtos::SleepFor(kb::Milliseconds{150U}); // the hub finishes its own power-on boot
  const kb_it::OwnerLoop<OwnerBus, JoinImu> owner{r.domain, r.io};
  KB_REQUIRE(r.io.EnsureStarted(kStart).IsOk());
  const kb::handlers::StartupReport rep = r.io.Startup().Value();
  KB_CHECK(rep.complete);
  KB_CHECK_EQ(rep.stages, 3U);
  KB_CHECK_EQ(JoinHandler::kStartupPlan[0].kinds, kb::handlers::stage::kLink);
  KB_CHECK(Eventually([&] { return Count(r.io, ::bno08x::SensorId::Accelerometer) > 2U; }, 500U));
  KB_CHECK(r.io.Stop(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Unknown);
}

} // namespace
