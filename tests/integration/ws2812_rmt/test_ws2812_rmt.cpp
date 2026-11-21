// ws2812_rmt through kb::handlers::ws2812_rmt: the start-up (channel, then the blank frame), the
// app API, error mapping to the health record, recovery, and both thread models. The pulse
// peripheral has no kb contract: the binding passes the driver's own port type through, here the
// driver's RMT model (components/drivers/driver-ws2812-rmt/tests/ mock_ws2812_rmt.hpp), as an MCU
// port would supply the ESP32 RMT channel. SPDX-License-Identifier: MIT Copyright (c) 2026 The
// kb-core authors
#include <cstddef>
#include <cstdint>

#include "binding_support.hpp"
#include "kb/handlers/bindings/ws2812_rmt.hpp"
#include "kb_test.hpp"
#include "mock_ws2812_rmt.hpp"

namespace {

using kb::handlers::HealthState;
namespace wsh = kb::handlers::ws2812_rmt;
using Hw = ws2812_rmt_test::FullPort;        // with the Power pin and Log
using BareHw = ws2812_rmt_test::MinimalPort; // the four required hooks only

constexpr std::size_t kPixels = 8U;
using Handler = wsh::Handler<Hw, kPixels>;
using GuardedBus = kb::handlers::BusDomain<Hw, kb::handlers::ThreadModel::Guarded>;
using OwnerBus = kb::handlers::BusDomain<Hw, kb::handlers::ThreadModel::Owner>;
using GuardedIo = kb::handlers::Guarded<Handler, GuardedBus>;
using OwnedIo = kb::handlers::OwnerAccess<Handler, OwnerBus>;

constexpr kb::Milliseconds kWait{200U};

wsh::Config StripConfig() {
  wsh::Config c{};
  c.chip.power_on_settle_us = 10U;
  c.handler.health.fault_after = 2U;
  c.handler.recovery.backoff = kb::Milliseconds{1U};
  return c;
}

/// A guarded handler on its own channel, created.
struct GuardedRig {
  Hw hw{};
  GuardedBus domain{hw};
  GuardedIo io;
  explicit GuardedRig(const wsh::Config& cfg) : io(domain, hw, cfg) {
    static_cast<void>(domain.Create());
    static_cast<void>(io.Create());
  }
};

/// The G, R, B bytes of pixel `i` of the last frame (the default WS2812B order is GRB).
bool PixelIs(const Hw& hw, std::size_t i, uint8_t r, uint8_t g, uint8_t b) {
  const std::size_t at = i * 3U;
  return (hw.last_wire.size() >= (at + 3U)) && (hw.last_wire[at] == g) &&
         (hw.last_wire[at + 1U] == r) && (hw.last_wire[at + 2U] == b);
}

constexpr ws2812_rmt::Colour kRed{255U, 0U, 0U, 0U};
constexpr ws2812_rmt::Colour kBlue{0U, 0U, 255U, 0U};

KB_TEST("ws2812_rmt: guarded start opens the channel, powers the strip and sends a blank frame") {
  GuardedRig r{StripConfig()};
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Unknown);
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  const kb::handlers::StartupReport rep = r.io.Startup().Value();
  KB_CHECK(rep.complete);
  KB_CHECK_EQ(rep.stages, 2U);
  KB_CHECK_EQ(rep.kinds, kb::handlers::stage::kConfigure);
  KB_CHECK(r.hw.powered);
  KB_CHECK_EQ(r.hw.transmit_calls, 1);
  KB_CHECK_EQ(r.hw.last_wire.size(), kPixels * 3U);
  KB_CHECK(PixelIs(r.hw, 0U, 0U, 0U, 0U));
  KB_CHECK(r.io.EnsureStarted(kWait).IsOk()); // idempotent
  KB_CHECK_EQ(r.hw.init_calls, 1);
}

KB_TEST("ws2812_rmt: without blank_on_start only the channel stage runs") {
  wsh::Config cfg = StripConfig();
  cfg.blank_on_start = false;
  GuardedRig r{cfg};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  KB_CHECK_EQ(r.hw.transmit_calls, 0);
  KB_CHECK(!r.hw.powered);
  KB_CHECK(wsh::Show(r.io, kWait).Error() == kb::Status::NotReady); // not powered yet
  KB_CHECK_EQ(r.io.Health().Value().last_detail,
              kb::handlers::DetailOf(ws2812_rmt::Error::NotPowered));
  KB_CHECK(wsh::SetPower(r.io, true, kWait).IsOk());
  KB_CHECK(wsh::Show(r.io, kWait).IsOk());
  KB_CHECK_EQ(r.hw.transmit_calls, 1);
}

KB_TEST("ws2812_rmt: calls before a start send nothing and return NotReady") {
  GuardedRig r{StripConfig()};
  KB_CHECK(wsh::FillStrip(r.io, kRed, kWait).Error() == kb::Status::NotReady);
  KB_CHECK(wsh::Show(r.io, kWait).Error() == kb::Status::NotReady);
  KB_CHECK(r.io.Sample(kWait).Error() ==
           kb::Status::Unsupported); // an actuator has no periodic read
  KB_CHECK_EQ(r.hw.init_calls + r.hw.transmit_calls + r.hw.power_calls, 0);
}

KB_TEST("ws2812_rmt: a channel that does not open or is too coarse fails stage 0; a retry starts "
        "again") {
  GuardedRig r{StripConfig()};
  r.hw.fail_init = true;
  const kb::Result<void> first = r.io.EnsureStarted(kWait);
  KB_CHECK(first.Error() == kb::Status::BusError);
  KB_CHECK_EQ(first.Detail(), kb::handlers::DetailOf(ws2812_rmt::Error::Bus));
  KB_CHECK_EQ(r.io.Startup().Value().stage, 0U);
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Faulted);
  r.hw.fail_init = false;
  r.hw.resolution_hz = 10'000'000U; // below kMinResolutionHz
  const kb::Result<void> second = r.io.EnsureStarted(kWait);
  KB_CHECK(second.Error() == kb::Status::Unsupported);
  KB_CHECK_EQ(second.Detail(), kb::handlers::DetailOf(ws2812_rmt::Error::TimingUnreachable));
  r.hw.resolution_hz = 20'000'000U;
  KB_CHECK(r.io.EnsureStarted(kWait).IsOk());
  KB_CHECK_EQ(r.io.Health().Value().start_failures, 2U);
}

KB_TEST("ws2812_rmt: pixels, fill, range, brightness, clear and power through the app API") {
  GuardedRig r{StripConfig()};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  KB_CHECK(wsh::FillStrip(r.io, kRed, kWait).IsOk());
  KB_CHECK(PixelIs(r.hw, 0U, 255U, 0U, 0U) && PixelIs(r.hw, 7U, 255U, 0U, 0U));
  KB_CHECK(wsh::SetPixel(r.io, 2U, kBlue, kWait, true).IsOk());
  KB_CHECK(PixelIs(r.hw, 2U, 0U, 0U, 255U) && PixelIs(r.hw, 3U, 255U, 0U, 0U));
  wsh::Command range{};
  range.op = wsh::Op::SetRange;
  range.index = 4U;
  range.count = 2U;
  range.colour = kBlue;
  range.show = true;
  KB_CHECK(r.io.Request(range, kWait).IsOk());
  KB_CHECK(PixelIs(r.hw, 4U, 0U, 0U, 255U) && PixelIs(r.hw, 5U, 0U, 0U, 255U) &&
           PixelIs(r.hw, 6U, 255U, 0U, 0U));
  wsh::Command get{};
  get.op = wsh::Op::GetPixel;
  get.index = 2U;
  const auto px = r.io.Request(get, kWait);
  KB_REQUIRE(px.IsOk());
  KB_CHECK(px.Value().colour == kBlue);
  KB_CHECK(px.Value().frame_us > 0U);
  KB_CHECK(wsh::SetStripBrightness(r.io, 0U, kWait).IsOk());
  KB_CHECK(wsh::Show(r.io, kWait).IsOk());
  KB_CHECK(PixelIs(r.hw, 2U, 0U, 0U, 0U)); // dimmed to black, the pixel buffer kept
  KB_CHECK(wsh::SetStripBrightness(r.io, 255U, kWait).IsOk());
  KB_CHECK(wsh::ClearStrip(r.io, kWait).IsOk());
  KB_CHECK(PixelIs(r.hw, 2U, 0U, 0U, 0U));
  KB_CHECK(wsh::SetPixel(r.io, kPixels, kRed, kWait).Error() == kb::Status::InvalidArgument);
  KB_CHECK(wsh::SetPower(r.io, false, kWait).IsOk());
  KB_CHECK(!r.hw.powered);
}

KB_TEST("ws2812_rmt: failed frames degrade, the threshold faults, Recover() restores Ready") {
  GuardedRig r{StripConfig()};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  r.hw.fail_transmit_at = r.hw.transmit_calls + 1;
  const kb::Result<void> bad = wsh::FillStrip(r.io, kRed, kWait);
  KB_CHECK(bad.Error() == kb::Status::BusError);
  KB_CHECK_EQ(bad.Detail(), kb::handlers::DetailOf(ws2812_rmt::Error::Bus));
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Degraded);
  KB_CHECK(wsh::Show(r.io, kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  r.hw.timeout_on_transmit = true;
  KB_CHECK(!wsh::Show(r.io, kWait).IsOk());
  KB_CHECK(!wsh::Show(r.io, kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Faulted);
  const int before = r.hw.transmit_calls;
  KB_CHECK(wsh::Show(r.io, kWait).Error() == kb::Status::NotReady);
  KB_CHECK_EQ(r.hw.transmit_calls, before); // a faulted handler sends nothing
  r.hw.timeout_on_transmit = false;
  KB_CHECK(r.io.RequestRecovery(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  KB_CHECK(r.hw.powered);
  KB_CHECK_EQ(r.io.Health().Value().recoveries, 1U);
}

KB_TEST("ws2812_rmt: a port without the Power pin starts powered; SetPower is Unsupported") {
  BareHw hw{};
  kb::handlers::BusDomain<BareHw, kb::handlers::ThreadModel::Guarded> domain{hw};
  kb::handlers::Guarded<wsh::Handler<BareHw, kPixels>, decltype(domain)> io{domain, hw,
                                                                            StripConfig()};
  KB_REQUIRE(domain.Create().IsOk());
  KB_REQUIRE(io.Create().IsOk());
  KB_REQUIRE(io.EnsureStarted(kWait).IsOk());
  KB_CHECK_EQ(hw.transmit_calls, 1);
  KB_CHECK(wsh::SetPower(io, true, kWait).Error() == kb::Status::Unsupported);
  KB_CHECK(wsh::FillStrip(io, kRed, kWait).IsOk());
  KB_CHECK_EQ(hw.transmit_calls, 2);
}

KB_TEST("ws2812_rmt: owner model: the owner task starts the strip and serves requests") {
  Hw hw{};
  OwnerBus domain{hw};
  OwnedIo io{domain, hw, StripConfig()};
  KB_REQUIRE(io.Create().IsOk());
  const kb_it::OwnerLoop<OwnerBus, OwnedIo> owner{domain, io};
  KB_REQUIRE(io.EnsureStarted(kWait).IsOk());
  KB_CHECK(wsh::FillStrip(io, kBlue, kWait).IsOk());
  KB_CHECK(wsh::SetPixel(io, 1U, kRed, kWait, true).IsOk());
  KB_CHECK(PixelIs(hw, 0U, 0U, 0U, 255U) && PixelIs(hw, 1U, 255U, 0U, 0U));
  KB_CHECK(io.Liveness().Beats() > 0U);
  KB_CHECK(io.Stop(kWait).IsOk());
  KB_CHECK(!hw.powered); // Stop() switches the strip supply off
  KB_CHECK(kb_it::StateOf(io) == HealthState::Unknown);
}

} // namespace
