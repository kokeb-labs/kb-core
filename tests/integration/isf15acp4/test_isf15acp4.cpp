// isf15acp4 through kb::handlers::isf15acp4: start-up, the app API, the switch through samples,
// error mapping to the health record, recovery, and both thread models, over a kb::iface::SpiDevice
// and kb pins backed by the driver's own controller model and rig
// (components/drivers/driver-isf15acp4/tests/mock_isf15acp4_bus.hpp). The model's clock follows the
// RTOS clock, so its timing rules (reset pulse, supply settle, engine busy) check the real waits.
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include <atomic>
#include <cstdint>
#include <span>

#include "binding_support.hpp"
#include "kb/handlers/bindings/isf15acp4.hpp"
#include "kb_test.hpp"
#include "mock_isf15acp4_bus.hpp"

namespace {

using kb::handlers::HealthState;
namespace kh = kb::handlers::isf15acp4;
using isf15acp4::CtrlPin;
using isf15acp4::DataKind;
using isf15acp4::GpioSignal;
using kb::iface::Level;

/// The driver's rig with its clock kept at the RTOS clock, and the D/C# level the pins set.
struct Board {
  isf15acp4_test::Rig rig{};
  bool dc_high{false};
  uint64_t last_us{kb::rtos::Now().Us()};
  void Sync() noexcept {
    const uint64_t now = kb::rtos::Now().Us();
    rig.chip.Elapse(static_cast<uint32_t>(now - last_us));
    last_us = now;
  }
};

/// kb::iface::SpiDevice: one write-only transfer to the controller model with the D/C# level of the
/// pin.
struct ModelSpi {
  Board& board;
  [[nodiscard]] kb::Status EnsureReady() noexcept {
    return board.rig.Init() ? kb::Status::Ok : kb::Status::NotReady;
  }
  [[nodiscard]] kb::Status Transfer(std::span<const uint8_t> tx, std::span<uint8_t> rx) noexcept {
    static_cast<void>(rx);
    board.Sync();
    const DataKind kind = board.dc_high ? DataKind::Data : DataKind::Command;
    return board.rig.Write(kind, tx.data(), tx.size()) ? kb::Status::Ok : kb::Status::BusError;
  }
};
static_assert(kb::iface::SpiDevice<ModelSpi>);

/// D/C#: a plain level.
struct DcPin {
  Board& board;
  [[nodiscard]] kb::Status Set(Level level) noexcept {
    board.dc_high = level == Level::High;
    return kb::Status::Ok;
  }
  [[nodiscard]] kb::Status Toggle() noexcept {
    board.dc_high = !board.dc_high;
    return kb::Status::Ok;
  }
};

/// RES# (low holds the controller in reset) or the supply enable (high is on).
struct CtrlOut {
  Board& board;
  CtrlPin pin;
  [[nodiscard]] kb::Status Set(Level level) noexcept {
    board.Sync();
    const bool active = (pin == CtrlPin::Reset) ? (level == Level::Low) : (level == Level::High);
    const bool ok = board.rig.Gpio(pin, active ? GpioSignal::ACTIVE : GpioSignal::INACTIVE);
    return ok ? kb::Status::Ok : kb::Status::BusError;
  }
  [[nodiscard]] kb::Status Toggle() noexcept {
    return kb::Status::Unsupported;
  }
};

/// The switch pin: the physical level.
struct SwitchIn {
  Board& board;
  /// Owner-model tests press the switch from the test thread through this atomic (-1: the model's
  /// level), as the model itself is not thread-safe.
  std::atomic<int> forced{-1};
  [[nodiscard]] kb::Status Get(Level& level) noexcept {
    bool high = false;
    const bool ok = board.rig.ReadSwitch(high);
    const int f = forced.load();
    high = (f < 0) ? high : (f != 0);
    level = high ? Level::High : Level::Low;
    return ok ? kb::Status::Ok : kb::Status::BusError;
  }
};

/// The model behind a SPI device and its four pins.
struct Hardware {
  Board board{};
  ModelSpi spi{board};
  DcPin dc{board};
  CtrlOut reset{board, CtrlPin::Reset};
  CtrlOut vcc{board, CtrlPin::VccEnable};
  SwitchIn sw{board};
  isf15acp4_test::Ssd1331Model& Chip() noexcept {
    return board.rig.chip;
  }
};

using Handler = kh::Handler<ModelSpi>;
using SpiTag = kb::handlers::BusTag<kb::handlers::BusKind::Spi>;
using GuardedBus = kb::handlers::BusDomain<SpiTag, kb::handlers::ThreadModel::Guarded>;
using OwnerBus = kb::handlers::BusDomain<SpiTag, kb::handlers::ThreadModel::Owner>;
using GuardedIo = kb::handlers::Guarded<Handler, GuardedBus>;
using OwnedIo = kb::handlers::OwnerAccess<Handler, OwnerBus>;

constexpr kb::Milliseconds kWait{2000U};

kh::Config Wired(Hardware& hw, bool with_switch = true) {
  kh::Config c{};
  c.outputs[kh::kPinDc].pin = kb::iface::GpioOutputRef{hw.dc};
  c.outputs[kh::kPinReset].pin = kb::iface::GpioOutputRef{hw.reset};
  c.outputs[kh::kPinReset].active_low = true;
  c.outputs[kh::kPinVcc].pin = kb::iface::GpioOutputRef{hw.vcc};
  if (with_switch) {
    c.inputs[kh::kPinSwitch].pin = kb::iface::GpioInputRef{hw.sw};
  }
  c.chip.vcc_settle_ms = 2U;
  c.chip.vcc_off_ms = 2U;
  c.handler.health.fault_after = 2U;
  c.handler.recovery.backoff = kb::Milliseconds{1U};
  return c;
}

/// A guarded handler on its own SPI device, created.
struct GuardedRig {
  kb_it::Timing t{};
  Hardware hw{};
  SpiTag tag{};
  GuardedBus domain{tag};
  GuardedIo io;
  explicit GuardedRig(bool with_switch = true)
      : io(domain, hw.spi, t.timing, Wired(hw, with_switch)) {
    static_cast<void>(domain.Create());
    static_cast<void>(io.Create());
  }
  isf15acp4_test::Rig& Rig() noexcept {
    return hw.board.rig;
  }
};

/// Samples until `done(snapshot)` or `ms` milliseconds passed.
template <typename Io, typename F>
bool SampleUntil(Io& io, uint32_t ms, F done) {
  bool ok = false;
  for (uint32_t i = 0U; (i < ms) && (!ok); ++i) {
    const auto s = io.Sample(kWait);
    ok = s.IsOk() && done(s.Value().value);
    kb::rtos::SleepFor(kb::Milliseconds{1U});
  }
  return ok;
}

KB_TEST("isf15acp4: guarded start runs the one sync stage and turns the panel on") {
  GuardedRig r{};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  const kb::handlers::StartupReport report = r.io.Startup().Value();
  KB_CHECK(report.complete);
  KB_CHECK_EQ(report.stages, 1U);
  KB_CHECK_EQ(report.kinds, kb::handlers::stage::kIdentify | kb::handlers::stage::kConfigure);
  KB_CHECK(r.hw.Chip().Vcc());
  KB_CHECK(r.hw.Chip().Lit());
  KB_CHECK_EQ(r.hw.Chip().Violations(),
              0U); // reset pulse, supply and RAM order as the controller needs
  const int writes = r.Rig().writes;
  KB_CHECK(r.io.EnsureStarted(kWait).IsOk()); // idempotent
  KB_CHECK_EQ(r.Rig().writes, writes);
}

KB_TEST("isf15acp4: calls before a start send nothing and return NotReady") {
  GuardedRig r{};
  KB_CHECK(kh::Clear(r.io, isf15acp4::kRed, kWait).Error() == kb::Status::NotReady);
  KB_CHECK(r.io.Sample(kWait).Error() == kb::Status::NotReady);
  KB_CHECK_EQ(r.Rig().writes, 0);
  KB_CHECK_EQ(r.Rig().gpio_calls, 0);
}

KB_TEST("isf15acp4: a reset pin that cannot be driven fails the start with BusError; a retry "
        "starts again") {
  GuardedRig r{};
  r.Rig().fail_reset_pin = true;
  const kb::Result<void> first = r.io.EnsureStarted(kWait);
  KB_CHECK(first.Error() == kb::Status::BusError);
  KB_CHECK_EQ(first.Detail(), kb::handlers::DetailOf(isf15acp4::Error::Bus));
  const kb::handlers::StartupReport rep = r.io.Startup().Value();
  KB_CHECK(!rep.complete);
  KB_CHECK(rep.status == kb::Status::BusError);
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Faulted);
  KB_CHECK(!r.hw.Chip().Vcc()); // the supply stays off
  r.Rig().fail_reset_pin = false;
  KB_CHECK(r.io.EnsureStarted(kWait).IsOk());
  KB_CHECK_EQ(r.io.Startup().Value().runs, 2U);
  KB_CHECK(r.hw.Chip().Lit());
}

KB_TEST("isf15acp4: drawing, contrast, mode and display power through the app API reach the "
        "controller") {
  GuardedRig r{};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  KB_CHECK(kh::Clear(r.io, isf15acp4::kRed, kWait).IsOk());
  KB_CHECK_EQ(r.hw.Chip().Rgb565At(10, 10), 0xF800U);
  KB_CHECK(kh::FillWindow(r.io, {0U, 0U, 4U, 4U}, isf15acp4::kGreen, kWait).IsOk());
  KB_CHECK_EQ(r.hw.Chip().Rgb565At(3, 3), 0x07E0U);
  KB_CHECK_EQ(r.hw.Chip().Rgb565At(4, 4), 0xF800U);
  kh::Command pixel{};
  pixel.op = kh::Op::SetPixel;
  pixel.x0 = 50U;
  pixel.y0 = 20U;
  pixel.color = isf15acp4::kBlue;
  KB_CHECK(r.io.Request(pixel, kWait).IsOk());
  KB_CHECK_EQ(r.hw.Chip().Rgb565At(50, 20), 0x001FU);
  KB_CHECK(kh::DrawLine(r.io, {0U, 30U, 20U, 30U}, isf15acp4::kWhite, kWait).IsOk());
  kh::Command rect{};
  rect.op = kh::Op::DrawRectangle;
  rect.x0 = 60U;
  rect.y0 = 40U;
  rect.x1 = 70U;
  rect.y1 = 50U;
  rect.color = isf15acp4::kWhite;
  KB_CHECK(r.io.Request(rect, kWait).IsOk());
  rect.op = kh::Op::FillRectangle;
  rect.fill = isf15acp4::kBlue;
  KB_CHECK(r.io.Request(rect, kWait).IsOk());
  KB_CHECK(kh::SetContrast(r.io, 0x30U, 0x30U, 0x30U, kWait).IsOk());
  KB_CHECK_EQ(r.hw.Chip().Contrast(0), 0x30U);
  kh::Command mode{};
  mode.op = kh::Op::SetDisplayMode;
  mode.mode = isf15acp4::DisplayMode::Inverse;
  KB_CHECK(r.io.Request(mode, kWait).IsOk());
  kh::Command dim{};
  dim.op = kh::Op::DisplayDim;
  KB_CHECK(r.io.Request(dim, kWait).IsOk());
  KB_CHECK(r.hw.Chip().DisplayState() == isf15acp4_test::Display::Dim);
  KB_CHECK(kh::DisplayOff(r.io, kWait).IsOk());
  KB_CHECK(!r.hw.Chip().Vcc());
  KB_CHECK(kh::DisplayOn(r.io, kWait).IsOk());
  KB_CHECK(r.hw.Chip().Lit());
  kh::Command activity{};
  activity.op = kh::Op::NotifyActivity;
  KB_CHECK(r.io.Request(activity, kWait).IsOk());
  kh::Command sw{};
  sw.op = kh::Op::ReadSwitch;
  const auto open = r.io.Request(sw, kWait);
  KB_REQUIRE(open.IsOk());
  KB_CHECK(!open.Value().closed);
  KB_CHECK_EQ(r.hw.Chip().Violations(), 0U);
}

KB_TEST("isf15acp4: samples debounce the switch and publish the press") {
  GuardedRig r{};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  KB_CHECK(SampleUntil(r.io, 100U, [](const kh::Snapshot& s) { return s.has_switch; }));
  const uint32_t before = r.io.Latest().Value().value.events;
  r.Rig().switch_high = false; // closed (pull-up wiring)
  KB_CHECK(SampleUntil(r.io, 200U, [before](const kh::Snapshot& s) { return s.events > before; }));
  const kh::Snapshot down = r.io.Latest().Value().value;
  KB_CHECK(down.has_switch);
  KB_CHECK_EQ(down.events, before + 1U);
  KB_CHECK(down.last_event.event == isf15acp4::ButtonEvent::Pressed);
  KB_CHECK(down.button_down);
  KB_CHECK_EQ(down.presses, 1U);
  KB_CHECK(down.output == isf15acp4::Output::Normal);
  KB_CHECK(down.supply_on);
  r.Rig().switch_high = true;
  KB_CHECK(SampleUntil(r.io, 200U,
                       [before](const kh::Snapshot& s) { return s.events > (before + 1U); }));
  KB_CHECK(r.io.Latest().Value().value.last_event.event == isf15acp4::ButtonEvent::Released);
}

KB_TEST("isf15acp4: without a switch input a sample reads the driver state only") {
  GuardedRig r{false};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  r.Rig().fail_switch = true; // never read
  const auto s = r.io.Sample(kWait);
  KB_REQUIRE(s.IsOk());
  KB_CHECK(!s.Value().value.has_switch);
  KB_CHECK(s.Value().value.supply_on);
}

KB_TEST("isf15acp4: a bad argument is refused, a failed transfer degrades, the threshold faults, "
        "Recover() "
        "restores Ready") {
  GuardedRig r{};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  const kb::Result<void> bad = kh::FillWindow(r.io, {200U, 0U, 4U, 4U}, isf15acp4::kRed, kWait);
  KB_CHECK(bad.Error() == kb::Status::InvalidArgument);
  KB_CHECK_EQ(bad.Detail(), kb::handlers::DetailOf(isf15acp4::Error::InvalidArgument));
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready); // refused, not counted against the chip
  KB_CHECK_EQ(r.io.Health().Value().refused_calls, 1U);
  KB_CHECK(kh::Clear(r.io, isf15acp4::kBlack, kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  r.Rig().fail_write_at = r.Rig().writes;
  const kb::Result<void> lost = kh::Clear(r.io, isf15acp4::kBlack, kWait);
  KB_CHECK(lost.Error() == kb::Status::BusError);
  KB_CHECK_EQ(lost.Detail(), kb::handlers::DetailOf(isf15acp4::Error::Bus));
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Degraded);
  r.Rig().fail_switch = true;
  KB_CHECK(r.io.Sample(kWait).Error() == kb::Status::BusError);
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Faulted);
  const int writes = r.Rig().writes;
  KB_CHECK(kh::Clear(r.io, isf15acp4::kBlack, kWait).Error() == kb::Status::NotReady);
  KB_CHECK_EQ(r.Rig().writes, writes); // a faulted handler sends nothing
  r.Rig().fail_switch = false;
  KB_CHECK(r.io.RequestRecovery(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  KB_CHECK_EQ(r.io.Health().Value().recoveries, 1U);
  KB_CHECK(r.hw.Chip().Lit()); // the plan turned the panel on again
}

KB_TEST("isf15acp4: the error map and the fatal set") {
  static_assert(kh::ToStatus(isf15acp4::Error::Ok) == kb::Status::Ok);
  static_assert(kh::ToStatus(isf15acp4::Error::Bus) == kb::Status::BusError);
  static_assert(kh::ToStatus(isf15acp4::Error::Locked) == kb::Status::Busy);
  static_assert(kh::ToStatus(isf15acp4::Error::SupplyOff) == kb::Status::NotReady);
  KB_CHECK(kh::IsFatalError(isf15acp4::Error::NotInitialized));
  KB_CHECK(!kh::IsFatalError(isf15acp4::Error::Bus));
}

KB_TEST("isf15acp4: owner model: the owner task starts, samples the switch and serves requests") {
  kb_it::Timing t{};
  Hardware hw{};
  SpiTag tag{};
  OwnerBus domain{tag};
  kh::Config cfg = Wired(hw);
  cfg.handler.sample_period = kb::Milliseconds{5U};
  OwnedIo io{domain, hw.spi, t.timing, cfg};
  KB_REQUIRE(io.Create().IsOk());
  {
    const kb_it::OwnerLoop<OwnerBus, OwnedIo> owner{domain, io};
    KB_REQUIRE(io.EnsureStarted(kWait).IsOk());
    KB_CHECK(kh::Clear(io, isf15acp4::kGreen, kWait).IsOk());
    kb::rtos::SleepFor(kb::Milliseconds{20U}); // the debouncer adopts the open switch first
    hw.sw.forced.store(0);
    bool pressed = false;
    for (uint32_t i = 0U; (i < 500U) && (!pressed); ++i) {
      const auto s = io.Latest();
      pressed = s.IsOk() && (s.Value().value.events == 1U);
      kb::rtos::SleepFor(kb::Milliseconds{1U});
    }
    KB_CHECK(pressed);
    KB_CHECK(io.Liveness().Beats() > 0U);
    KB_CHECK(io.Stop(kWait).IsOk());
    KB_CHECK(kb_it::StateOf(io) == HealthState::Unknown);
  }
  KB_CHECK_EQ(hw.Chip().Rgb565At(0, 0), 0x07E0U);
  KB_CHECK(!hw.Chip().Vcc()); // Stop() is the driver's power-down
}

} // namespace
