// kb::dma::Stream with a real producer thread (the interrupt) and a consumer thread (the owner
// task): frames arrive in order, a frame the consumer holds never changes, every frame is delivered
// or counted as dropped, and the stall check never treats a frame stamped after the task read its
// clock as a stall. Run under ThreadSanitizer by the gates. SPDX-License-Identifier: MIT Copyright
// (c) 2026 The kb-core authors
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <utility>

#include "kb/dma/stream.hpp"
#include "kb_test.hpp"

namespace d = kb::dma;

namespace {

struct Region {
  static constexpr d::CachePolicy kPolicy = d::CachePolicy::NonCacheable;
  // cppcheck-suppress unusedStructMember ; read through the template parameter
  static constexpr std::size_t kLineBytes = 1U;
  // cppcheck-suppress unusedStructMember ; read through the template parameter
  static constexpr std::size_t kAlignBytes = 4U;
  static constexpr d::MasterSet kMasters = 1U;
};

struct Frame {
  std::array<uint32_t, 8> v{};
};

} // namespace

KB_TEST("threads: a producer and a consumer lose nothing silently and never see a torn slot") {
  constexpr uint32_t kFrames = 20000U;
  d::Stream<Frame, 4U, Region> stream{d::StreamConfig{.frame_period = kb::Microseconds{100U},
                                                      .stall_periods = 10U,
                                                      .bad_frames_to_restart = 0U,
                                                      .restart_backoff = kb::Milliseconds{1U},
                                                      .restarts_before_fallback = 0U}};
  stream.Start(kb::Instant{0U});
  std::atomic<bool> done{false};
  std::atomic<uint64_t> clock{0U};
  std::atomic<uint32_t> bad{0U};

  std::thread producer([&]() {
    for (uint32_t i = 0U; i < kFrames; ++i) {
      Frame f{};
      for (std::size_t k = 0U; k < f.v.size(); ++k) {
        f.v[k] = i + static_cast<uint32_t>(k);
      }
      clock.store(i, std::memory_order_release);
      static_cast<void>(stream.Publish(f, kb::Instant{i}, 0U));
      if ((i % 64U) == 0U) {
        std::this_thread::yield();
      }
    }
    done.store(true, std::memory_order_release);
  });

  uint32_t delivered = 0U;
  bool have_last = false;
  uint32_t last = 0U;
  for (;;) {
    // The task reads its clock, then the interrupt stamps a frame: that is never a stall.
    if (stream.CheckStall(kb::Instant{clock.load(std::memory_order_acquire)})) {
      bad.fetch_add(1U, std::memory_order_relaxed);
    }
    d::Stream<Frame, 4U, Region>::FrameView view;
    if (stream.TryTake(view) == kb::Status::Ok) {
      const uint32_t first = view.Data().v[0];
      if (have_last && (first <= last)) {
        bad.fetch_add(1U, std::memory_order_relaxed);
      }
      have_last = true;
      last = first;
      if ((delivered % 7U) == 0U) {
        std::this_thread::yield(); // hold it while the producer runs
      }
      for (std::size_t k = 1U; k < view.Data().v.size(); ++k) {
        if (view.Data().v[k] != (first + static_cast<uint32_t>(k))) {
          bad.fetch_add(1U, std::memory_order_relaxed);
        }
      }
      if (view.Seq() != first) {
        bad.fetch_add(1U, std::memory_order_relaxed);
      }
      ++delivered;
    } else if (done.load(std::memory_order_acquire)) {
      break;
    } else {
      std::this_thread::yield();
    }
  }
  producer.join();
  d::Stream<Frame, 4U, Region>::FrameView rest;
  while (stream.TryTake(rest) == kb::Status::Ok) {
    ++delivered;
    stream.Give(std::move(rest));
  }
  const d::StreamStats stats = stream.Stats();
  KB_CHECK_EQ(bad.load(), 0U);
  KB_CHECK_EQ(stats.published, kFrames);
  KB_CHECK_EQ(stats.delivered + stats.overruns, kFrames);
  KB_CHECK_EQ(stats.delivered, delivered);
}
