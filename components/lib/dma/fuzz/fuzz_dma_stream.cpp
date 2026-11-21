// Fuzz target: kb::dma::Stream follows a model of its ring for any sequence of producer and
// consumer operations (components/lib/dma/fuzz/README.md): frames arrive in order with strictly
// increasing sequence numbers, a held frame never changes, every dropped frame is counted and
// flags the next delivered one, an overflow is never a frame. Single context (the threaded test
// covers the interleaving). Loops stay in the plain form of the target code.
//
// cppcheck-suppress-file useStlAlgorithm
//
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <utility>

#include "kb/dma/stream.hpp"

namespace {

void Require(bool condition) {
  if (!condition) {
    std::abort();
  }
}

struct Region {
  static constexpr kb::dma::CachePolicy kPolicy = kb::dma::CachePolicy::NonCacheable;
  // cppcheck-suppress unusedStructMember ; read through the template parameter
  static constexpr std::size_t kLineBytes = 1U;
  // cppcheck-suppress unusedStructMember ; read through the template parameter
  static constexpr std::size_t kAlignBytes = 4U;
  static constexpr kb::dma::MasterSet kMasters = 1U;
};

struct Frame {
  std::array<uint32_t, 2> v{};
};

constexpr std::size_t kFrames = 3U;
using S = kb::dma::Stream<Frame, kFrames, Region>;

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, std::size_t size) {
  S stream{kb::dma::StreamConfig{.frame_period = kb::Microseconds{100U},
                                 .stall_periods = 4U,
                                 .bad_frames_to_restart = 3U,
                                 .restart_backoff = kb::Milliseconds{1U},
                                 .restarts_before_fallback = 2U}};
  S::FrameView held;
  stream.Start(kb::Instant{0U});
  uint64_t now = 0U;
  uint32_t reported = 0U; // frames the producer reported (the next sequence number)
  uint32_t dropped = 0U;  // frames dropped because the ring was full
  uint32_t delivered = 0U;
  bool have_last = false;
  uint32_t last_seq = 0U;
  uint32_t held_a = 0U;
  uint32_t held_b = 0U;
  for (std::size_t i = 0U; i < size; ++i) {
    now += 7U + (data[i] >> 4U);
    switch (data[i] & 0x7U) {
    case 0U:
    case 1U: {
      const uint32_t seq = reported;
      const kb::Status status = stream.Publish(
          Frame{{seq, ~seq}}, kb::Instant{now}, 0U,
          ((data[i] & 0x8U) != 0U) ? kb::dma::Integrity::Torn : kb::dma::Integrity::Clean);
      ++reported;
      if (status == kb::Status::Full) {
        ++dropped;
      } else {
        Require(status == kb::Status::Ok);
      }
      break;
    }
    case 2U:
      stream.OnOverrun(1U);
      break;
    case 3U:
    case 4U: {
      S::FrameView view;
      const kb::Status status = stream.TryTake(view);
      if (status == kb::Status::Ok) {
        Require(!held.Valid());
        Require(view.Data().v[1] == ~view.Data().v[0]);
        Require(view.Data().v[0] == view.Seq());
        if (have_last) {
          Require(view.Seq() > last_seq);
          if (view.Seq() != (last_seq + 1U)) {
            Require(view.Gap());
          }
        }
        have_last = true;
        last_seq = view.Seq();
        ++delivered;
        held_a = view.Data().v[0];
        held_b = view.Data().v[1];
        held = std::move(view);
      } else {
        Require((status == kb::Status::Empty) || (status == kb::Status::Busy));
        Require((status == kb::Status::Busy) == held.Valid());
      }
      break;
    }
    case 5U:
      if (held.Valid()) {
        Require(held.Data().v[0] == held_a);
        Require(held.Data().v[1] == held_b);
        held = S::FrameView{}; // releases the slot
      }
      break;
    case 6U:
      static_cast<void>(stream.CheckStall(kb::Instant{now}));
      break;
    default:
      static_cast<void>(stream.Supervise(kb::Instant{now}));
      break;
    }
    if (held.Valid()) {
      Require(held.Data().v[0] == held_a);
      Require(held.Data().v[1] == held_b);
    }
  }
  const kb::dma::StreamStats stats = stream.Stats();
  Require(stats.published == reported);
  Require(stats.overruns == dropped);
  Require(stats.delivered == delivered);
  Require(stats.delivered + stats.overruns <= stats.published);
  return 0;
}
