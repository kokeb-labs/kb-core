// kb::dma::Stream: delivery order, the three overrun checks, torn frames, content checks, the
// signed stall check, the supervisor (restart, back-off, fallback), PublishFromDevice and the frame
// view's ownership. SPDX-License-Identifier: MIT Copyright (c) 2026 The kb-core authors
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <utility>

#include "kb/dma/buffer.hpp"
#include "kb/dma/stream.hpp"
#include "kb_test.hpp"

namespace d = kb::dma;
using kb::Instant;
using kb::Status;

namespace {

struct Region4 {
  static constexpr d::CachePolicy kPolicy = d::CachePolicy::NonCacheable;
  // cppcheck-suppress unusedStructMember ; read through the template parameter
  static constexpr std::size_t kLineBytes = 1U;
  // cppcheck-suppress unusedStructMember ; read through the template parameter
  static constexpr std::size_t kAlignBytes = 4U;
  static constexpr d::MasterSet kMasters = 1U;
};
struct CachedRegion {
  static constexpr d::CachePolicy kPolicy = d::CachePolicy::Maintained;
  // cppcheck-suppress unusedStructMember ; read through the template parameter
  static constexpr std::size_t kLineBytes = 32U;
  // cppcheck-suppress unusedStructMember ; read through the template parameter
  static constexpr std::size_t kAlignBytes = 32U;
  static constexpr d::MasterSet kMasters = 1U;
};

struct Frame {
  std::array<uint16_t, 4> v{};
};

using S2 = d::Stream<Frame, 2U, Region4>;
using S3 = d::Stream<Frame, 3U, Region4>;

Frame Make(uint16_t base) {
  return Frame{{base, static_cast<uint16_t>(base + 1U), static_cast<uint16_t>(base + 2U),
                static_cast<uint16_t>(base + 3U)}};
}

d::StreamConfig Config() {
  return d::StreamConfig{.frame_period = kb::Microseconds{1000U},
                         .stall_periods = 10U,
                         .bad_frames_to_restart = 3U,
                         .restart_backoff = kb::Milliseconds{5U},
                         .restarts_before_fallback = 2U};
}

struct Port {
  [[nodiscard]] static d::PortKey<Port> Key() noexcept {
    return d::PortKey<Port>{};
  }
};

} // namespace

KB_TEST("stream: frames come out in order with sequence numbers and timestamps") {
  S3 s{Config()};
  s.Start(Instant{0U});
  KB_CHECK_EQ(s.Publish(Make(0U), Instant{100U}, 7U), Status::Ok);
  KB_CHECK_EQ(s.Publish(Make(4U), Instant{200U}, 8U), Status::Ok);
  S3::FrameView v;
  KB_CHECK(!v.Valid());
  KB_REQUIRE(s.TryTake(v) == Status::Ok);
  KB_CHECK(v.Valid());
  KB_CHECK_EQ(v.Seq(), 0U);
  KB_CHECK_EQ(v.At().Us(), uint64_t{100U});
  KB_CHECK_EQ(v.HwFlags(), 7U);
  KB_CHECK_EQ(v.Data().v[0], uint16_t{0U});
  KB_CHECK_EQ(v.Get()->v[3], uint16_t{3U});
  KB_CHECK(!v.Gap() && !v.Torn() && !v.Fallback());
  KB_CHECK_EQ(v.Epoch(), 0U);
  S3::FrameView again;
  KB_CHECK_EQ(s.TryTake(again), Status::Busy);
  s.Give(std::move(v));
  KB_CHECK(!v.Valid()); // NOLINT(bugprone-use-after-move,clang-analyzer-cplusplus.Move): a
                        // moved-from lease or view is empty
  KB_REQUIRE(s.TryTake(again) == Status::Ok);
  KB_CHECK_EQ(again.Seq(), 1U);
  s.Give(std::move(again));
  KB_CHECK_EQ(s.TryTake(again), Status::Empty);
  KB_CHECK_EQ(s.Stats().delivered, 2U);
  KB_CHECK_EQ(s.Stats().published, 2U);
  KB_CHECK(s.LastFrameAt().Us() == uint64_t{200U});
}

KB_TEST(
    "stream: a view releases its slot when it ends, and a move assignment releases the old one") {
  S3 s{Config()};
  KB_CHECK_EQ(s.Publish(Make(0U), Instant{1U}, 0U), Status::Ok);
  KB_CHECK_EQ(s.Publish(Make(4U), Instant{2U}, 0U), Status::Ok);
  {
    S3::FrameView v;
    KB_REQUIRE(s.TryTake(v) == Status::Ok);
  }
  S3::FrameView a;
  KB_REQUIRE(s.TryTake(a) == Status::Ok);
  KB_CHECK_EQ(a.Seq(), 1U);
  S3::FrameView b{std::move(a)};
  // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  KB_CHECK(!a.Valid());
  KB_CHECK(b.Valid());
  S3::FrameView c;
  c = std::move(b);
  KB_CHECK(c.Valid());
  // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  KB_CHECK(!b.Valid());
  KB_CHECK_EQ(s.Publish(Make(8U), Instant{3U}, 0U), Status::Ok);
  S3::FrameView e;
  KB_CHECK_EQ(s.TryTake(e), Status::Busy);
  c = S3::FrameView{};
  KB_REQUIRE(s.TryTake(e) == Status::Ok);
  KB_CHECK_EQ(e.Seq(), 2U);
}

KB_TEST("stream: with every slot held a frame is dropped, counted, and the next one is a gap with "
        "a larger sequence number") {
  S2 s{Config()};
  KB_CHECK_EQ(s.Publish(Make(0U), Instant{1U}, 0U), Status::Ok);
  S2::FrameView held;
  KB_REQUIRE(s.TryTake(held) == Status::Ok);
  KB_CHECK_EQ(s.Publish(Make(4U), Instant{2U}, 0U), Status::Ok);
  KB_CHECK_EQ(s.Publish(Make(8U), Instant{3U}, 0U), Status::Full);
  KB_CHECK_EQ(s.Publish(Make(12U), Instant{4U}, 0U), Status::Full);
  KB_CHECK_EQ(s.Stats().overruns, 2U);
  KB_CHECK_EQ(held.Data().v[0], uint16_t{0U});
  s.Give(std::move(held));
  KB_CHECK_EQ(s.Publish(Make(16U), Instant{5U}, 0U), Status::Ok);
  S2::FrameView v;
  KB_REQUIRE(s.TryTake(v) == Status::Ok);
  KB_CHECK_EQ(v.Seq(), 1U);
  KB_CHECK(!v.Gap());
  s.Give(std::move(v));
  KB_REQUIRE(s.TryTake(v) == Status::Ok);
  KB_CHECK_EQ(v.Seq(), 4U);
  KB_CHECK(v.Gap());
  KB_CHECK_EQ(s.Stats().gaps, 1U);
}

KB_TEST("stream: a hardware overrun is counted, never a frame, and flags the next frame") {
  S2 s{Config()};
  s.OnOverrun(5U);
  S2::FrameView v;
  KB_CHECK_EQ(s.TryTake(v), Status::Empty);
  KB_CHECK_EQ(s.Stats().hw_overruns, 1U);
  KB_CHECK_EQ(s.Stats().last_hw_flags, 5U);
  KB_CHECK_EQ(s.Publish(Make(0U), Instant{1U}, 0U), Status::Ok);
  KB_REQUIRE(s.TryTake(v) == Status::Ok);
  KB_CHECK(v.Gap());
  s.Give(std::move(v));
  KB_CHECK_EQ(s.Publish(Make(4U), Instant{2U}, 0U), Status::Ok);
  KB_REQUIRE(s.TryTake(v) == Status::Ok);
  KB_CHECK(!v.Gap());
}

KB_TEST("stream: a torn frame is delivered flagged and counted") {
  S2 s{Config()};
  KB_CHECK_EQ(s.Publish(Make(0U), Instant{1U}, 0U, d::Integrity::Torn), Status::Ok);
  S2::FrameView v;
  KB_REQUIRE(s.TryTake(v) == Status::Ok);
  KB_CHECK(v.Torn());
  KB_CHECK_EQ(s.Stats().torn, 1U);
}

KB_TEST("stream: the sequence numbers survive more frames than the ring holds, for a ring size "
        "that is not a power of two") {
  S3 s{Config()};
  uint32_t expected = 0U;
  for (uint16_t i = 0U; i < 20U; ++i) {
    KB_CHECK_EQ(s.Publish(Make(i), Instant{i}, 0U), Status::Ok);
    S3::FrameView v;
    KB_REQUIRE(s.TryTake(v) == Status::Ok);
    KB_CHECK_EQ(v.Seq(), expected);
    KB_CHECK_EQ(v.Data().v[0], i);
    ++expected;
  }
}

KB_TEST("stream: the stall check uses signed time and counts an episode once") {
  S2 s{Config()};
  KB_CHECK(!s.CheckStall(Instant{100000U}));
  s.Start(Instant{0U});
  KB_CHECK_EQ(s.Publish(Make(0U), Instant{1050U}, 0U), Status::Ok);
  KB_CHECK(!s.CheckStall(Instant{1000U}));
  KB_CHECK(!s.CheckStall(Instant{1050U + 9999U}));
  KB_CHECK(s.CheckStall(Instant{1050U + 10000U}));
  KB_CHECK(s.CheckStall(Instant{1050U + 20000U}));
  KB_CHECK_EQ(s.Stats().stalls, 1U);
  S2::FrameView v;
  KB_REQUIRE(s.TryTake(v) == Status::Ok);
  KB_CHECK(s.CheckStall(Instant{1050U + 40000U}));
  KB_CHECK_EQ(s.Stats().stalls, 2U);
  s.Stop();
  KB_CHECK(!s.CheckStall(Instant{1050U + 90000U}));
  KB_CHECK(!s.Stats().running);
  S2 off{d::StreamConfig{}};
  off.Start(Instant{0U});
  KB_CHECK(!off.CheckStall(Instant{1000000U}));
}

KB_TEST("stream: content judgements restart after enough bad frames, and a good frame resets the "
        "count") {
  S2 s{Config()};
  s.Start(Instant{0U});
  KB_CHECK_EQ(s.Publish(Make(0U), Instant{10U}, 0U), Status::Ok);
  S2::FrameView v;
  KB_REQUIRE(s.TryTake(v) == Status::Ok);
  const auto bad = [](const Frame&) noexcept { return false; };
  const auto good = [](const Frame&) noexcept { return true; };
  S2::FrameView none;
  KB_CHECK(!s.Judge(none, good));
  s.NoteContent(true);
  KB_CHECK(!s.Judge(v, bad));
  KB_CHECK(!s.Judge(v, bad));
  KB_CHECK(s.Supervise(Instant{20U}) == d::SupervisorAction::None);
  KB_CHECK(s.Judge(v, good));
  KB_CHECK(!s.Judge(v, bad));
  KB_CHECK(!s.Judge(v, bad));
  KB_CHECK(s.Supervise(Instant{20U}) == d::SupervisorAction::None);
  s.NoteContent(false);
  KB_CHECK(s.Supervise(Instant{20U}) == d::SupervisorAction::Restart);
  KB_CHECK_EQ(s.Stats().bad_frames, 6U);
  S2 never{d::StreamConfig{.frame_period = kb::Microseconds{0U},
                           .stall_periods = 10U,
                           .bad_frames_to_restart = 0U,
                           .restart_backoff = kb::Milliseconds{5U},
                           .restarts_before_fallback = 0U}};
  never.Start(Instant{0U});
  for (int i = 0; i < 20; ++i) {
    never.NoteContent(false);
  }
  KB_CHECK(never.Supervise(Instant{20U}) == d::SupervisorAction::None);
}

KB_TEST("stream: restart, back-off, fallback, return to DMA, each with a new epoch") {
  S2 s{d::StreamConfig{.frame_period = kb::Microseconds{1000U},
                       .stall_periods = 10U,
                       .bad_frames_to_restart = 3U,
                       .restart_backoff = kb::Milliseconds{50U},
                       .restarts_before_fallback = 2U}};
  KB_CHECK(s.Supervise(Instant{0U}) == d::SupervisorAction::None);
  s.Start(Instant{0U});
  KB_CHECK(s.Supervise(Instant{5000U}) == d::SupervisorAction::None);
  KB_CHECK(s.Supervise(Instant{10000U}) == d::SupervisorAction::Restart);
  s.NoteRestart(Instant{10000U});
  KB_CHECK_EQ(s.Stats().restarts, 1U);
  KB_CHECK_EQ(s.Stats().epoch, 1U);
  KB_CHECK(s.Supervise(Instant{20000U}) == d::SupervisorAction::BackOff);
  KB_CHECK(s.Supervise(Instant{60000U}) == d::SupervisorAction::Restart);
  s.NoteRestart(Instant{60000U});
  KB_CHECK(s.Supervise(Instant{70000U}) == d::SupervisorAction::UseFallback);
  s.EnterFallback(Instant{70000U});
  KB_CHECK(s.Mode() == d::StreamMode::Fallback);
  KB_CHECK_EQ(s.Stats().fallbacks, 1U);
  KB_CHECK_EQ(s.Publish(Make(0U), Instant{71000U}, 0U), Status::Ok);
  S2::FrameView v;
  KB_REQUIRE(s.TryTake(v) == Status::Ok);
  KB_CHECK(v.Fallback());
  KB_CHECK_EQ(v.Epoch(), 3U);
  s.Give(std::move(v));
  KB_CHECK(s.Supervise(Instant{200000U}) == d::SupervisorAction::Restart);
  s.ReturnToDma(Instant{200000U});
  KB_CHECK(s.Mode() == d::StreamMode::Dma);
  KB_CHECK_EQ(s.Stats().epoch, 4U);
  KB_CHECK(s.Stats().mode == d::StreamMode::Dma);
}

KB_TEST("stream: interrupt cost keeps its maximum") {
  S2 s{Config()};
  s.NoteIsrCycles(100U);
  s.NoteIsrCycles(50U);
  s.NoteIsrCycles(300U);
  KB_CHECK_EQ(s.Stats().max_isr_cycles, 300U);
}

KB_TEST("stream: PublishFromDevice invalidates a cached frame's lines, copies it out and checks "
        "its arguments") {
  using SC = d::Stream<Frame, 2U, CachedRegion>;
  static_assert(SC::kFrameStride == 32U);
  static_assert(SC::kHardwareBytes == 64U);
  SC s{Config()};
  SC::HardwareBuffer hw;
  struct Ops {
    int invalidates{0};
    int barriers{0};
    static void Clean(std::span<const std::byte> range) noexcept {
      static_cast<void>(range);
    }
    void Invalidate(std::span<std::byte> range) noexcept {
      static_cast<void>(range);
      ++invalidates;
    }
    void Barrier() noexcept {
      ++barriers;
    }
  } ops;
  d::Lease<CachedRegion> lease;
  KB_CHECK_EQ(s.PublishFromDevice(Port::Key(), ops, lease, 0U, d::Completion{Instant{1U}, 0U}),
              Status::InvalidArgument);
  KB_CHECK_EQ(hw.Lend(Port::Key(), d::Direction::ToDevice, SC::kHardwareBytes, ops, lease),
              Status::Ok);
  KB_CHECK_EQ(s.PublishFromDevice(Port::Key(), ops, lease, 0U, d::Completion{Instant{1U}, 0U}),
              Status::InvalidArgument);
  KB_CHECK_EQ(lease.Complete(Port::Key(), ops, d::Outcome::Ok), Status::Ok);
  KB_CHECK_EQ(hw.Lend(Port::Key(), d::Direction::FromDevice, SC::kHardwareBytes, ops, lease),
              Status::Ok);
  const int before = ops.invalidates;
  const auto device = lease.DeviceView(Port::Key());
  const Frame sent = Make(40U);
  for (std::size_t i = 0U; i < sizeof(Frame); ++i) {
    device[32U + i] = std::bit_cast<std::array<std::byte, sizeof(Frame)>>(sent)[i];
  }
  KB_CHECK_EQ(s.PublishFromDevice(Port::Key(), ops, lease, 2U, d::Completion{Instant{1U}, 0U}),
              Status::InvalidArgument);
  KB_CHECK_EQ(s.PublishFromDevice(Port::Key(), ops, lease, 1U,
                                  d::Completion{Instant{9U}, 3U, d::Integrity::Torn}),
              Status::Ok);
  KB_CHECK_EQ(ops.invalidates, before + 1);
  S2::FrameView unused;
  static_cast<void>(unused);
  SC::FrameView v;
  KB_REQUIRE(s.TryTake(v) == Status::Ok);
  KB_CHECK_EQ(v.Data().v[0], uint16_t{40U});
  KB_CHECK(v.Torn());
  KB_CHECK_EQ(lease.Abort(Port::Key(), ops, d::StopEvidence::Confirmed), Status::Ok);
}

KB_TEST("stream: PublishFromDevice in an uncached region does not invalidate") {
  S2 s{Config()};
  S2::HardwareBuffer hw;
  d::NoCacheOps ops;
  d::Lease<Region4> lease;
  KB_CHECK_EQ(hw.Lend(Port::Key(), d::Direction::FromDevice, S2::kHardwareBytes, ops, lease),
              Status::Ok);
  KB_CHECK_EQ(s.PublishFromDevice(Port::Key(), ops, lease, 0U, d::Completion{Instant{1U}, 0U}),
              Status::Ok);
  KB_CHECK_EQ(lease.Complete(Port::Key(), ops, d::Outcome::Ok), Status::Ok);
}

namespace {

d::StreamConfig Cfg(uint32_t period_us, uint32_t stall_periods, uint32_t bad, uint32_t backoff_ms,
                    uint32_t fallback_after) {
  return d::StreamConfig{.frame_period = kb::Microseconds{period_us},
                         .stall_periods = stall_periods,
                         .bad_frames_to_restart = bad,
                         .restart_backoff = kb::Milliseconds{backoff_ms},
                         .restarts_before_fallback = fallback_after};
}

} // namespace

KB_TEST("stream: defaults of the records and of a fresh stream") {
  const d::StreamConfig c{};
  KB_CHECK_EQ(c.frame_period.Count(), uint64_t{0U});
  KB_CHECK_EQ(c.stall_periods, 10U);
  KB_CHECK_EQ(c.bad_frames_to_restart, 8U);
  KB_CHECK_EQ(c.restart_backoff.Count(), uint64_t{1000U});
  KB_CHECK_EQ(c.restarts_before_fallback, 0U);
  const d::StreamStats z{};
  S2 s;
  for (const d::StreamStats& t : {z, s.Stats()}) {
    KB_CHECK_EQ(t.published, 0U);
    KB_CHECK_EQ(t.delivered, 0U);
    KB_CHECK_EQ(t.overruns, 0U);
    KB_CHECK_EQ(t.gaps, 0U);
    KB_CHECK_EQ(t.torn, 0U);
    KB_CHECK_EQ(t.hw_overruns, 0U);
    KB_CHECK_EQ(t.bad_frames, 0U);
    KB_CHECK_EQ(t.stalls, 0U);
    KB_CHECK_EQ(t.restarts, 0U);
    KB_CHECK_EQ(t.fallbacks, 0U);
    KB_CHECK_EQ(t.max_isr_cycles, 0U);
    KB_CHECK_EQ(t.epoch, 0U);
    KB_CHECK_EQ(t.last_hw_flags, 0U);
    KB_CHECK(t.mode == d::StreamMode::Dma);
    KB_CHECK(!t.running);
  }
  KB_CHECK_EQ(s.LastFrameAt().Us(), uint64_t{0U});
  S2::FrameView v;
  KB_CHECK(!v.Valid());
  KB_CHECK_EQ(v.Seq(), 0U);
  KB_CHECK_EQ(v.HwFlags(), 0U);
  KB_CHECK_EQ(v.Epoch(), 0U);
  KB_CHECK_EQ(v.At().Us(), uint64_t{0U});
  KB_CHECK(!v.Gap() && !v.Torn() && !v.Fallback());
  KB_CHECK(v.Get() == nullptr);
  static_assert(static_cast<uint8_t>(d::SupervisorAction::None) == 0U &&
                static_cast<uint8_t>(d::SupervisorAction::Restart) == 1U &&
                static_cast<uint8_t>(d::SupervisorAction::BackOff) == 2U &&
                static_cast<uint8_t>(d::SupervisorAction::UseFallback) == 3U);
  static_assert(static_cast<uint8_t>(d::StreamMode::Dma) == 0U &&
                static_cast<uint8_t>(d::StreamMode::Fallback) == 1U);
  static_assert(static_cast<uint8_t>(d::Integrity::Clean) == 0U &&
                static_cast<uint8_t>(d::Integrity::Torn) == 1U);
}

KB_TEST("stream: the sample instant divides exactly and handles a rate of one") {
  KB_CHECK_EQ(d::SampleInstant(kb::Instant{9000000U}, 5U, 0U, 3U).Us(),
              uint64_t{9000000U - 1666666U});
  KB_CHECK_EQ(d::SampleInstant(kb::Instant{9000000U}, 2U, 0U, 1U).Us(), uint64_t{7000000U});
}

KB_TEST(
    "stream: the stall check with a zero budget fires one microsecond after the stamp, not at it") {
  S2 s{Cfg(1000U, 0U, 0U, 1U, 0U)};
  s.Start(Instant{5U});
  KB_CHECK(!s.CheckStall(Instant{5U}));
  KB_CHECK(s.CheckStall(Instant{6U}));
}

KB_TEST("stream: a new stall episode after a restart is counted again") {
  S2 s{Cfg(1000U, 10U, 0U, 1U, 0U)};
  s.Start(Instant{0U});
  KB_CHECK(s.CheckStall(Instant{10000U}));
  KB_CHECK_EQ(s.Stats().stalls, 1U);
  s.NoteRestart(Instant{10000U});
  KB_CHECK(!s.CheckStall(Instant{10001U}));
  KB_CHECK(s.CheckStall(Instant{20000U}));
  KB_CHECK_EQ(s.Stats().stalls, 2U);
}

KB_TEST("stream: Start clears the bad run, a restart clears it, a good frame clears the restart "
        "count") {
  S2 s{Cfg(1000U, 10U, 1U, 1000U, 2U)};
  s.NoteContent(false);
  s.Start(Instant{0U});
  KB_CHECK(s.Supervise(Instant{1U}) == d::SupervisorAction::None);
  s.NoteContent(false);
  KB_CHECK(s.Supervise(Instant{1U}) == d::SupervisorAction::Restart);
  s.NoteRestart(Instant{1U});
  KB_CHECK(s.Supervise(Instant{1U}) == d::SupervisorAction::None);
  // One restart, a good frame, one more restart: the count restarted, so no fallback yet.
  s.NoteContent(true);
  s.NoteRestart(Instant{2U});
  s.NoteContent(false);
  KB_CHECK(s.Supervise(Instant{3U + 1000000U}) == d::SupervisorAction::Restart);
}

KB_TEST("stream: entering the fallback clears the restart count, and one restart before fallback "
        "is honoured") {
  S2 s{Cfg(1000U, 10U, 1U, 0U, 1U)};
  s.Start(Instant{0U});
  s.NoteContent(false);
  KB_CHECK(s.Supervise(Instant{1U}) == d::SupervisorAction::Restart);
  s.NoteRestart(Instant{1U});
  s.NoteContent(false);
  KB_CHECK(s.Supervise(Instant{1U}) == d::SupervisorAction::UseFallback);
  s.EnterFallback(Instant{1U});
  s.ReturnToDma(Instant{2U});
  s.NoteContent(false);
  KB_CHECK(s.Supervise(Instant{2U}) == d::SupervisorAction::Restart);
}

KB_TEST(
    "stream: a restart window of zero never backs off, and the window ends exactly at its length") {
  S2 zero{Cfg(1000U, 10U, 1U, 0U, 0U)};
  zero.Start(Instant{0U});
  zero.NoteRestart(Instant{100U});
  zero.NoteContent(false);
  KB_CHECK(zero.Supervise(Instant{100U}) == d::SupervisorAction::Restart);
  S2 w{Cfg(1000U, 10U, 1U, 5U, 0U)};
  w.Start(Instant{0U});
  w.NoteRestart(Instant{100U});
  w.NoteContent(false);
  KB_CHECK(w.Supervise(Instant{100U + 4999U}) == d::SupervisorAction::BackOff);
  KB_CHECK(w.Supervise(Instant{100U + 5000U}) == d::SupervisorAction::Restart);
  KB_CHECK(w.Supervise(Instant{50U}) == d::SupervisorAction::BackOff);
}

KB_TEST("stream: PublishFromDevice refuses a lease smaller than the stream's hardware buffer") {
  S2 s{Cfg(1000U, 10U, 0U, 1U, 0U)};
  d::Buffer<Region4, 4U> small;
  d::NoCacheOps ops;
  d::Lease<Region4> lease;
  KB_CHECK_EQ(small.Lend(Port::Key(), d::Direction::FromDevice, 4U, ops, lease), Status::Ok);
  KB_CHECK_EQ(s.PublishFromDevice(Port::Key(), ops, lease, 0U, d::Completion{Instant{1U}, 0U}),
              Status::InvalidArgument);
  KB_CHECK_EQ(lease.Complete(Port::Key(), ops, d::Outcome::Ok), Status::Ok);
}

KB_TEST("stream: frame times above 32 bits survive the two-word stamp and the stall check") {
  constexpr uint64_t kBase = (uint64_t{1U} << 33U) + 7U;
  S2 s{d::StreamConfig{.frame_period = kb::Microseconds{100U}, .stall_periods = 10U}};
  s.Start(Instant{kBase});
  KB_CHECK_EQ(s.LastFrameAt().Us(), kBase);
  KB_CHECK_EQ(s.Publish(Frame{}, Instant{kBase + 50U}, 0U), Status::Ok);
  KB_CHECK_EQ(s.LastFrameAt().Us(), kBase + 50U);
  KB_CHECK(!s.CheckStall(Instant{kBase + 1049U}));
  KB_CHECK(s.CheckStall(Instant{kBase + 1050U}));
  // A restart after the last frame re-arms from the restart time.
  s.NoteRestart(Instant{kBase + 2000U});
  KB_CHECK_EQ(s.LastFrameAt().Us(), kBase + 2000U);
  KB_CHECK(!s.CheckStall(Instant{kBase + 2999U}));
  // A frame one microsecond after the arm time is already the later one.
  KB_CHECK_EQ(s.Publish(Frame{}, Instant{kBase + 2001U}, 0U), Status::Ok);
  KB_CHECK_EQ(s.LastFrameAt().Us(), kBase + 2001U);
}

// The stream's shared state needs only 32-bit lock-free atomics, which every supported core has
// without a runtime library (a 64-bit atomic would need libatomic on 32-bit cores).
static_assert(std::atomic<uint32_t>::is_always_lock_free);

KB_TEST("buffer: holds no address of itself, so a zeroed section is its start state") {
  static d::Buffer<Region4, 12U> b;
  KB_CHECK(b.Current() == d::BufferState::Cpu);
  KB_CHECK_EQ(b.Capacity(), std::size_t{12U});
  KB_CHECK_EQ(b.CpuView().size(), std::size_t{12U});
  KB_CHECK_EQ(b.Stats().lends, 0U);
}
