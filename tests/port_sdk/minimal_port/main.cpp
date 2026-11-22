// A minimal port made only from the port SDK: one GPIO pin class that satisfies
// the kb::iface contracts, and the DMA conformance suite T1 to T18 run against
// the host model through the SDK's headers. A real port provides its own rig
// with the same member functions (docs/port-sdk.md). SPDX-License-Identifier:
// MIT Copyright (c) 2026 The kb-core authors
#include <cstdint>

#include "kb/dma/host/conformance.hpp"
#include "kb/dma/host/rig.hpp"
#include "kb/dma/host/stream_rig.hpp"
#include "kb/dma/stream.hpp"
#include "kb/interfaces/gpio.hpp"
#include "kb/interfaces/host/fake_pins.hpp"
#include "kb_test.hpp"

namespace d = kb::dma;
namespace h = kb::dma::host;
namespace c = kb::dma::host::conformance;

namespace {

/// The whole "hardware": one pin.
class MinimalPin {
public:
  kb::Status Set(kb::iface::Level level) noexcept {
    level_ = level;
    return kb::Status::Ok;
  }
  kb::Status Toggle() noexcept {
    level_ = (level_ == kb::iface::Level::High) ? kb::iface::Level::Low : kb::iface::Level::High;
    return kb::Status::Ok;
  }
  kb::Status Get(kb::iface::Level& out) noexcept {
    out = level_;
    return kb::Status::Ok;
  }

private:
  kb::iface::Level level_{kb::iface::Level::Low};
};

static_assert(kb::iface::GpioOutput<MinimalPin> && kb::iface::GpioInput<MinimalPin>);

bool Passed(const char* verdict) {
  return verdict == nullptr;
}

constexpr d::StreamConfig kCfg{.frame_period = kb::Microseconds{1000U},
                               .stall_periods = 10U,
                               .bad_frames_to_restart = 8U,
                               .restart_backoff = kb::Milliseconds{1000U},
                               .restarts_before_fallback = 0U};

template <typename Case>
bool OnRig(Case fn, const h::RigOptions& opt = h::RigOptions{}) {
  h::HostRig rig(h::Faults{}, opt);
  return Passed(fn(rig));
}

template <typename Case>
bool OnStream(Case fn) {
  d::Stream<h::Scan, 2U, h::ModelUncached> stream(kCfg);
  h::AdcRig<d::Stream<h::Scan, 2U, h::ModelUncached>> rig(stream, h::Faults{});
  return Passed(fn(rig));
}

} // namespace

KB_TEST("the pin works through the contract") {
  MinimalPin pin{};
  kb::iface::Level now = kb::iface::Level::Low;
  KB_CHECK(pin.Toggle() == kb::Status::Ok);
  KB_CHECK(pin.Get(now) == kb::Status::Ok);
  KB_CHECK(now == kb::iface::Level::High);
  kb::iface::host::FakeGpioPin fake{};
  static_cast<void>(fake);
}

KB_TEST("DMA conformance T1 to T8, T14 to T16, T18 pass") {
  h::RigOptions round4{};
  round4.engine.round_rx_to = 4U;
  h::RigOptions stuck{};
  stuck.engine.stuck = true;
  KB_CHECK(OnRig([](auto& r) { return c::T1TxNeedsClean(r); }));
  KB_CHECK(OnRig([](auto& r) { return c::T2RxNeedsInvalidate(r); }));
  KB_CHECK(OnRig([](auto& r) { return c::T3CpuPathNoMaintenance(r); }));
  KB_CHECK(OnRig([](auto& r) { return c::T4NeighbourSurvives(r); }));
  KB_CHECK(OnRig([](auto& r) { return c::T5NoCpuDuringLease(r); }));
  KB_CHECK(OnRig([](auto& r) { return c::T6NoWriteAfterTimeout(r); }));
  KB_CHECK(OnRig([](auto& r) { return c::T7AbortWaitsForEnable(r); }));
  KB_CHECK(OnRig([](auto& r) { return c::T8RoundingStaysInside(r); }, round4));
  KB_CHECK(OnRig([](auto& r) { return c::T14ErrorFlags(r); }));
  KB_CHECK(OnRig([](auto& r) { return c::T15CancelRacesCompletion(r); }));
  KB_CHECK(OnRig([](auto& r) { return c::T16UnconfirmedQuarantines(r); }, stuck));
  KB_CHECK(OnRig([](auto& r) { return c::T18SyncThroughStaging(r); }, round4));
}

KB_TEST("DMA conformance T17: a fallback is reported") {
  h::RigOptions no_dma{};
  no_dma.port.has_dma = false;
  KB_CHECK(
      OnRig([&](auto& r) { return c::T17FallbackIsReported(r, no_dma.port.it_ceiling); }, no_dma));
}

KB_TEST("DMA conformance T9 to T13 (the stream) pass") {
  KB_CHECK(OnStream([](auto& r) { return c::T9HeldFrame(r); }));
  KB_CHECK(OnStream([](auto& r) { return c::T10LappedIsFlagged(r); }));
  KB_CHECK(OnStream([](auto& r) { return c::T11SingleDrainer(r); }));
  KB_CHECK(OnStream([](auto& r) { return c::T12SignedStall(r); }));
  KB_CHECK(OnStream([](auto& r) { return c::T13OverflowIsNotData(r); }));
}
