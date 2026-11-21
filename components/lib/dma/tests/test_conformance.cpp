// The conformance suite T1-T18: each case passes on the good model and fails on a model with the
// matching defect (the case is shown to detect it). SPDX-License-Identifier: MIT Copyright (c) 2026
// The kb-core authors
#include <cstdio>

#include "kb/dma/host/conformance.hpp"
#include "kb/dma/host/rig.hpp"
#include "kb/dma/host/stream_rig.hpp"
#include "kb/dma/stream.hpp"
#include "kb_test.hpp"

namespace d = kb::dma;
namespace h = kb::dma::host;
namespace c = kb::dma::host::conformance;

namespace {

/// True when the case passed; a failing case's text goes to stderr.
bool Passed(const char* verdict) {
  if (verdict != nullptr) {
    static_cast<void>(std::fputs("    verdict: ", stderr));
    static_cast<void>(std::fputs(verdict, stderr));
    static_cast<void>(std::fputs("\n", stderr));
  }
  return verdict == nullptr;
}

h::RigOptions RoundTo(std::size_t n) {
  h::RigOptions o{};
  o.engine.round_rx_to = n;
  return o;
}
h::RigOptions Stuck() {
  h::RigOptions o{};
  o.engine.stuck = true;
  return o;
}
h::RigOptions NoDma() {
  h::RigOptions o{};
  o.port.has_dma = false;
  return o;
}

template <typename Case>
void Run(Case fn, const h::Faults& bad, const h::RigOptions& opt = h::RigOptions{}) {
  {
    h::HostRig good(h::Faults{}, opt);
    KB_CHECK(Passed(fn(good)));
  }
  {
    h::HostRig broken(bad, opt);
    KB_CHECK(fn(broken) != nullptr);
  }
}

h::Faults With(bool h::Faults::* member) {
  h::Faults f{};
  f.*member = true;
  return f;
}

} // namespace

KB_TEST("conformance T1: transmit needs a clean") {
  Run([](auto& r) { return c::T1TxNeedsClean(r); }, With(&h::Faults::skip_clean));
}
KB_TEST("conformance T2: receive needs an invalidate before and after") {
  Run([](auto& r) { return c::T2RxNeedsInvalidate(r); }, With(&h::Faults::skip_invalidate_all));
  Run([](auto& r) { return c::T2RxNeedsInvalidate(r); }, With(&h::Faults::skip_invalidate_after));
}
KB_TEST("conformance T3: the CPU path does no maintenance") {
  Run([](auto& r) { return c::T3CpuPathNoMaintenance(r); }, With(&h::Faults::cpu_path_invalidates));
}
KB_TEST("conformance T4: a neighbour on a shared line survives") {
  Run([](auto& r) { return c::T4NeighbourSurvives(r); }, With(&h::Faults::shared_lines));
}
KB_TEST("conformance T5: no CPU view during a lease") {
  Run([](auto& r) { return c::T5NoCpuDuringLease(r); }, With(&h::Faults::view_during_lease));
}
KB_TEST("conformance T6: no DMA write after a timeout") {
  Run([](auto& r) { return c::T6NoWriteAfterTimeout(r); }, With(&h::Faults::timeout_returns_early));
}
KB_TEST("conformance T7: an abort waits for the enable bit") {
  Run([](auto& r) { return c::T7AbortWaitsForEnable(r); }, With(&h::Faults::abort_no_wait));
}
KB_TEST("conformance T8: receive rounding stays inside the buffer") {
  Run([](auto& r) { return c::T8RoundingStaysInside(r); }, With(&h::Faults::unpadded_staging),
      RoundTo(4U));
}
KB_TEST("conformance T14: error flags") {
  Run([](auto& r) { return c::T14ErrorFlags(r); }, With(&h::Faults::report_ok_on_error));
  Run([](auto& r) { return c::T14ErrorFlags(r); }, With(&h::Faults::global_flag_clear));
}
KB_TEST("conformance T15: a cancel racing a completion") {
  Run([](auto& r) { return c::T15CancelRacesCompletion(r); }, With(&h::Faults::double_report));
}
KB_TEST("conformance T16: an unconfirmed stop quarantines") {
  Run([](auto& r) { return c::T16UnconfirmedQuarantines(r); },
      With(&h::Faults::unconfirmed_returns_cpu), Stuck());
}
KB_TEST("conformance T17: fallback is reported") {
  const h::RigOptions o = NoDma();
  Run([&](auto& r) { return c::T17FallbackIsReported(r, o.port.it_ceiling); },
      With(&h::Faults::silent_fallback), o);
}
KB_TEST("conformance T18: synchronous transfer through staging") {
  Run([](auto& r) { return c::T18SyncThroughStaging(r); }, With(&h::Faults::dma_into_caller),
      RoundTo(4U));
}

namespace {

constexpr d::StreamConfig kCfg{.frame_period = kb::Microseconds{1000U},
                               .stall_periods = 10U,
                               .bad_frames_to_restart = 8U,
                               .restart_backoff = kb::Milliseconds{1000U},
                               .restarts_before_fallback = 0U};
using Real = d::Stream<h::Scan, 2U, h::ModelUncached>;
using Naive = h::NaiveStream<h::Scan, 2U>;

template <typename Case>
void RunStream(Case fn, const h::Faults& rig_bad, const h::StreamFaults& naive_bad) {
  {
    Real s(kCfg);
    h::AdcRig<Real> r(s, h::Faults{});
    KB_CHECK(Passed(fn(r)));
  }
  {
    Naive s(kCfg, naive_bad);
    h::AdcRig<Naive> r(s, rig_bad);
    KB_CHECK(fn(r) != nullptr);
  }
}

} // namespace

KB_TEST("conformance T9: a held frame is stable and the overrun shows") {
  RunStream([](auto& r) { return c::T9HeldFrame(r); }, h::Faults{},
            h::StreamFaults{.silent_overrun = true});
}
KB_TEST("conformance T10: a lapped half is flagged") {
  RunStream([](auto& r) { return c::T10LappedIsFlagged(r); }, With(&h::Faults::ignore_lapped),
            h::StreamFaults{});
}
KB_TEST("conformance T11: one drainer") {
  RunStream([](auto& r) { return c::T11SingleDrainer(r); }, With(&h::Faults::two_drainers),
            h::StreamFaults{});
}
KB_TEST("conformance T12: the stall check is signed") {
  RunStream([](auto& r) { return c::T12SignedStall(r); }, h::Faults{},
            h::StreamFaults{.unsigned_stall = true});
}
KB_TEST("conformance T13: an overflow is not data") {
  RunStream([](auto& r) { return c::T13OverflowIsNotData(r); }, h::Faults{},
            h::StreamFaults{.overflow_as_data = true});
}
