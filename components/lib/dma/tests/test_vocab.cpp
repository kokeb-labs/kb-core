// kb::dma vocabulary: regions, rounding, the compile-time helpers, Buffer and Lease and their state
// machine, the cache hooks per policy, the completion latch and the capability types.
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "kb/dma/buffer.hpp"
#include "kb/dma/capabilities.hpp"
#include "kb/dma/completion.hpp"
#include "kb/dma/dma.hpp"
#include "kb_test.hpp"

namespace d = kb::dma;
using kb::Status;

namespace {

struct Cached {
  static constexpr d::CachePolicy kPolicy = d::CachePolicy::Maintained;
  // cppcheck-suppress unusedStructMember ; read through the template parameter
  static constexpr std::size_t kLineBytes = 32U;
  // cppcheck-suppress unusedStructMember ; read through the template parameter
  static constexpr std::size_t kAlignBytes = 32U;
  static constexpr d::MasterSet kMasters = d::MasterBit(0U) | d::MasterBit(2U);
};
struct Plain {
  static constexpr d::CachePolicy kPolicy = d::CachePolicy::NonCacheable;
  // cppcheck-suppress unusedStructMember ; read through the template parameter
  static constexpr std::size_t kLineBytes = 1U;
  // cppcheck-suppress unusedStructMember ; read through the template parameter
  static constexpr std::size_t kAlignBytes = 4U;
  static constexpr d::MasterSet kMasters = d::MasterBit(0U);
};
struct Wide {
  static constexpr d::CachePolicy kPolicy = d::CachePolicy::Coherent;
  // cppcheck-suppress unusedStructMember ; read through the template parameter
  static constexpr std::size_t kLineBytes = 1U;
  // cppcheck-suppress unusedStructMember ; read through the template parameter
  static constexpr std::size_t kAlignBytes = 1U;
  static constexpr d::MasterSet kMasters = 1U;
};
struct BadLine {
  static constexpr d::CachePolicy kPolicy = d::CachePolicy::Coherent;
  // cppcheck-suppress unusedStructMember ; read through the template parameter
  static constexpr std::size_t kLineBytes = 3U;
  // cppcheck-suppress unusedStructMember ; read through the template parameter
  static constexpr std::size_t kAlignBytes = 4U;
  static constexpr d::MasterSet kMasters = 1U;
};
struct SmallAlign {
  static constexpr d::CachePolicy kPolicy = d::CachePolicy::Coherent;
  // cppcheck-suppress unusedStructMember ; read through the template parameter
  static constexpr std::size_t kLineBytes = 8U;
  // cppcheck-suppress unusedStructMember ; read through the template parameter
  static constexpr std::size_t kAlignBytes = 4U;
  static constexpr d::MasterSet kMasters = 1U;
};
struct MaintainedNoLine {
  static constexpr d::CachePolicy kPolicy = d::CachePolicy::Maintained;
  // cppcheck-suppress unusedStructMember ; read through the template parameter
  static constexpr std::size_t kLineBytes = 1U;
  // cppcheck-suppress unusedStructMember ; read through the template parameter
  static constexpr std::size_t kAlignBytes = 4U;
  static constexpr d::MasterSet kMasters = 1U;
};

static_assert(d::Region<Cached>);
static_assert(d::Region<Plain>);
static_assert(!d::Region<BadLine>);
static_assert(!d::Region<SmallAlign>);
static_assert(!d::Region<MaintainedNoLine>);
static_assert(!d::Region<int>);
static_assert(d::kRegionReachableBy<d::MasterBit(0U), Cached>);
static_assert(d::kRegionReachableBy<d::MasterBit(0U) | d::MasterBit(2U), Cached>);
static_assert(!d::kRegionReachableBy<d::MasterBit(1U), Cached>);
static_assert(d::RoundUp(1U, 32U) == 32U && d::RoundUp(32U, 32U) == 32U &&
              d::RoundUp(33U, 32U) == 64U);
static_assert(d::IsMultipleOf(64U, 32U) && !d::IsMultipleOf(65U, 32U));
static_assert(d::MasterBit(40U) == 0U);

// The compile-time helpers accept the good case (the bad ones are the compile-fail tests).
struct Elem4 {
  // cppcheck-suppress unusedStructMember ; only its size is checked
  std::array<std::byte, 4> b{};
};
[[maybe_unused]] constexpr bool kHelpersAccept = [] {
  d::RequireFrameFits<64U, 128U>();
  d::RequireReachable<d::MasterBit(0U), Cached>();
  d::RequireWholeUnits<64U, Cached>();
  d::RequireElementBytes<Elem4, 4U>();
  return true;
}();

struct CapPort {
  [[nodiscard]] static d::Capabilities Capabilities() noexcept {
    return d::Capabilities{};
  }
};
static_assert(d::ReportsCapabilities<CapPort>);
static_assert(!d::ReportsCapabilities<int>);

struct Recorder {
  std::vector<std::pair<std::size_t, std::size_t>> cleans;
  std::vector<std::pair<std::size_t, std::size_t>> invalidates;
  int barriers{0};
  const std::byte* origin{nullptr};
  void Clean(std::span<const std::byte> r) noexcept {
    cleans.emplace_back(static_cast<std::size_t>(r.data() - origin), r.size());
  }
  void Invalidate(std::span<std::byte> r) noexcept {
    invalidates.emplace_back(static_cast<std::size_t>(r.data() - origin), r.size());
  }
  void Barrier() noexcept {
    ++barriers;
  }
};
static_assert(d::CacheOps<Recorder>);
static_assert(d::CacheOps<d::NoCacheOps>);

struct Port {
  [[nodiscard]] static d::PortKey<Port> Key() noexcept {
    return d::PortKey<Port>{};
  }
};

} // namespace

KB_TEST("buffer: size is padded to the region unit and the data is aligned") {
  d::Buffer<Cached, 40U> b;
  static_assert(decltype(b)::kCapacity == 64U);
  static_assert(decltype(b)::kAlign == 32U);
  KB_CHECK_EQ(b.CpuView().size(), std::size_t{40U});
  KB_CHECK_EQ(b.Header().Capacity(), std::size_t{64U});
  KB_CHECK_EQ(d::AddressOf(b.Header().Base()) % 32U, std::uintptr_t{0U});
  const auto& cb = b;
  KB_CHECK_EQ(cb.CpuView().size(), std::size_t{40U});
  KB_CHECK_EQ(b.Verify(d::AddressOf(b.Header().Base()) - 64U, 4096U), Status::Ok);
  KB_CHECK_EQ(b.Verify(d::AddressOf(b.Header().Base()) + 64U, 4096U), Status::OutOfRange);
  KB_CHECK_EQ(b.Verify(d::AddressOf(b.Header().Base()) - 64U, 32U), Status::OutOfRange);
  d::Buffer<Plain, 5U> p;
  KB_CHECK_EQ(p.Header().Capacity(), std::size_t{8U});
}

KB_TEST("buffer: Verify rejects a misaligned placement") {
  d::Buffer<Wide, 3U> b;
  static_assert(decltype(b)::kAlign == 1U);
  KB_CHECK_EQ(b.Verify(0U, ~std::size_t{0U} / 2U), Status::Ok);
}

KB_TEST("lease: a transmit lend cleans the used lines, completion returns the memory") {
  d::Buffer<Cached, 100U> b;
  Recorder rec;
  rec.origin = b.Header().Base();
  d::Lease<Cached> lease;
  KB_CHECK(!lease.Active());
  KB_CHECK_EQ(b.Lend(Port::Key(), d::Direction::ToDevice, 40U, rec, lease), Status::Ok);
  KB_CHECK(lease.Active());
  KB_CHECK_EQ(lease.Used(), std::size_t{40U});
  KB_CHECK(lease.Dir() == d::Direction::ToDevice);
  KB_CHECK(b.Current() == d::BufferState::Device);
  KB_REQUIRE(rec.cleans.size() == 1U);
  KB_CHECK_EQ(rec.cleans[0].first, std::size_t{0U});
  KB_CHECK_EQ(rec.cleans[0].second, std::size_t{64U});
  KB_CHECK(rec.invalidates.empty());
  KB_CHECK(b.CpuView().empty());
  KB_CHECK_EQ(b.Stats().cpu_denied, 1U);
  KB_CHECK_EQ(lease.DeviceView(Port::Key()).size(), std::size_t{128U});
  KB_CHECK_EQ(lease.Complete(Port::Key(), rec, d::Outcome::Ok), Status::Ok);
  KB_CHECK(!lease.Active());
  KB_CHECK(b.Current() == d::BufferState::Cpu);
  KB_CHECK(rec.invalidates.empty());
  KB_CHECK_EQ(b.Stats().completions, 1U);
  KB_CHECK_EQ(lease.Complete(Port::Key(), rec, d::Outcome::Ok), Status::InvalidArgument);
  KB_CHECK_EQ(lease.Abort(Port::Key(), rec, d::StopEvidence::Confirmed), Status::InvalidArgument);
  KB_CHECK(lease.DeviceView(Port::Key()).empty());
}

KB_TEST("lease: a receive lend cleans then invalidates the whole buffer and invalidates the used "
        "lines on return") {
  d::Buffer<Cached, 100U> b;
  Recorder rec;
  rec.origin = b.Header().Base();
  d::Lease<Cached> lease;
  KB_CHECK_EQ(b.Lend(Port::Key(), d::Direction::FromDevice, 33U, rec, lease), Status::Ok);
  KB_REQUIRE(rec.cleans.size() == 1U);
  KB_CHECK_EQ(rec.cleans[0].second, std::size_t{128U});
  KB_REQUIRE(rec.invalidates.size() == 1U);
  KB_CHECK_EQ(rec.invalidates[0].second, std::size_t{128U});
  KB_CHECK_EQ(lease.Complete(Port::Key(), rec, d::Outcome::Error), Status::Ok);
  KB_REQUIRE(rec.invalidates.size() == 2U);
  KB_CHECK_EQ(rec.invalidates[1].second, std::size_t{64U});
  KB_CHECK_EQ(b.Stats().errors, 1U);
  KB_CHECK(rec.barriers >= 2);
}

KB_TEST("lease: an uncached region needs no line maintenance, only the barrier") {
  d::Buffer<Plain, 10U> b;
  Recorder rec;
  rec.origin = b.Header().Base();
  d::Lease<Plain> lease;
  KB_CHECK_EQ(b.Lend(Port::Key(), d::Direction::FromDevice, 10U, rec, lease), Status::Ok);
  KB_CHECK_EQ(lease.Complete(Port::Key(), rec, d::Outcome::Ok), Status::Ok);
  KB_CHECK(rec.cleans.empty() && rec.invalidates.empty());
  KB_CHECK_EQ(rec.barriers, 2);
}

KB_TEST("buffer: Lend rejects bad sizes, a second lease and a busy buffer") {
  d::Buffer<Plain, 10U> b;
  d::NoCacheOps ops;
  d::Lease<Plain> a;
  d::Lease<Plain> c;
  KB_CHECK_EQ(b.Lend(Port::Key(), d::Direction::ToDevice, 0U, ops, a), Status::InvalidArgument);
  KB_CHECK_EQ(b.Lend(Port::Key(), d::Direction::ToDevice, 11U, ops, a), Status::InvalidArgument);
  KB_CHECK_EQ(b.Lend(Port::Key(), d::Direction::ToDevice, 10U, ops, a), Status::Ok);
  KB_CHECK_EQ(b.Lend(Port::Key(), d::Direction::ToDevice, 10U, ops, a), Status::InvalidArgument);
  KB_CHECK_EQ(b.Lend(Port::Key(), d::Direction::ToDevice, 10U, ops, c), Status::Busy);
  KB_CHECK_EQ(b.Recover(Port::Key(), ops, d::StopEvidence::Confirmed), Status::Busy);
  KB_CHECK_EQ(a.Abort(Port::Key(), ops, d::StopEvidence::Confirmed), Status::Ok);
  KB_CHECK_EQ(b.Stats().aborts, 1U);
  KB_CHECK_EQ(b.Recover(Port::Key(), ops, d::StopEvidence::Confirmed), Status::InvalidArgument);
}

KB_TEST("lease: a moved lease keeps the buffer on the device, and the source is inactive") {
  d::Buffer<Plain, 10U> b;
  d::NoCacheOps ops;
  d::Lease<Plain> a;
  KB_CHECK_EQ(b.Lend(Port::Key(), d::Direction::ToDevice, 4U, ops, a), Status::Ok);
  d::Lease<Plain> moved{std::move(a)};
  KB_CHECK(!a.Active()); // NOLINT(bugprone-use-after-move,clang-analyzer-cplusplus.Move): a
                         // moved-from lease or view is empty
  KB_CHECK(moved.Active());
  KB_CHECK_EQ(moved.Used(), std::size_t{4U});
  KB_CHECK(b.Current() == d::BufferState::Device);
  KB_CHECK_EQ(moved.Complete(Port::Key(), ops, d::Outcome::Ok), Status::Ok);
}

KB_TEST("lease: an unconfirmed abort quarantines and only a proven recovery returns the buffer") {
  d::Buffer<Cached, 40U> b;
  Recorder rec;
  rec.origin = b.Header().Base();
  d::Lease<Cached> lease;
  KB_CHECK_EQ(b.Lend(Port::Key(), d::Direction::FromDevice, 40U, rec, lease), Status::Ok);
  KB_CHECK_EQ(lease.Abort(Port::Key(), rec, d::StopEvidence::NotConfirmed), Status::Fault);
  KB_CHECK(!lease.Active());
  KB_CHECK(b.Current() == d::BufferState::Quarantined);
  KB_CHECK(b.CpuView().empty());
  KB_CHECK_EQ(b.Lend(Port::Key(), d::Direction::ToDevice, 4U, rec, lease), Status::Fault);
  KB_CHECK_EQ(b.Recover(Port::Key(), rec, d::StopEvidence::NotConfirmed), Status::Fault);
  KB_CHECK(b.Current() == d::BufferState::Quarantined);
  const std::size_t before = rec.invalidates.size();
  KB_CHECK_EQ(b.Recover(Port::Key(), rec, d::StopEvidence::Confirmed), Status::Ok);
  KB_CHECK_EQ(rec.invalidates.size(), before + 1U);
  KB_CHECK(b.Current() == d::BufferState::Cpu);
  KB_CHECK_EQ(b.Stats().quarantines, 1U);
  KB_CHECK_EQ(b.Stats().recoveries, 1U);
  KB_CHECK_EQ(b.CpuView().size(), std::size_t{40U});
}

KB_TEST("lease: destroying an active lease quarantines the buffer") {
  d::Buffer<Plain, 10U> b;
  d::NoCacheOps ops;
  {
    d::Lease<Plain> lease;
    KB_CHECK_EQ(b.Lend(Port::Key(), d::Direction::FromDevice, 10U, ops, lease), Status::Ok);
  }
  KB_CHECK(b.Current() == d::BufferState::Quarantined);
  KB_CHECK_EQ(b.Stats().dropped, 1U);
  KB_CHECK_EQ(b.Stats().quarantines, 1U);
  KB_CHECK_EQ(b.Recover(Port::Key(), ops, d::StopEvidence::Confirmed), Status::Ok);
}

KB_TEST("latch: complete once, report once") {
  d::CompletionLatch l;
  KB_CHECK(l.Idle());
  KB_CHECK(l.Arm());
  KB_CHECK(!l.Arm());
  KB_CHECK(l.Pending());
  KB_CHECK(!l.TakeReport());
  KB_CHECK(l.TryComplete());
  KB_CHECK(!l.TryComplete());
  KB_CHECK(l.CompletionWaiting());
  KB_CHECK(l.TakeReport());
  KB_CHECK(!l.TakeReport());
  KB_CHECK(l.Idle());
}

KB_TEST("latch: a cancel claims a pending transfer and drops the completion that follows") {
  d::CompletionLatch l;
  KB_CHECK(l.TryCancel() == d::CancelClaim::Nothing);
  KB_CHECK(l.Arm());
  KB_CHECK(l.TryCancel() == d::CancelClaim::CancelledPending);
  KB_CHECK(!l.TryComplete());
  KB_CHECK(l.TryCancel() == d::CancelClaim::Nothing);
  l.FinishCancel();
  KB_CHECK(l.Idle());
  l.FinishCancel();
  KB_CHECK(l.Idle());
}

KB_TEST("latch: a cancel after an unreported completion drops the report") {
  d::CompletionLatch l;
  KB_CHECK(l.Arm());
  KB_CHECK(l.TryComplete());
  KB_CHECK(l.TryCancel() == d::CancelClaim::DroppedCompletion);
  KB_CHECK(!l.TakeReport());
  l.FinishCancel();
  KB_CHECK(l.Arm());
}

KB_TEST("capabilities: names, defaults and the health record") {
  KB_CHECK(d::ToString(d::TransferMode::Dma)[0] == 'd');
  KB_CHECK(d::ToString(d::TransferMode::Interrupt)[0] == 'i');
  KB_CHECK(d::ToString(d::TransferMode::Polled)[0] == 'p');
  // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
  const auto bogus = static_cast<d::TransferMode>(9);
  KB_CHECK(d::ToString(bogus)[0] == 'u');
  const d::Capabilities caps{};
  KB_CHECK(caps.mode == d::TransferMode::Polled);
  KB_CHECK(!caps.zero_copy);
  const d::DmaHealth health{};
  KB_CHECK(!health.faulted);
}

KB_TEST("stream helpers: signed elapsed time and the sample instant") {
  KB_CHECK_EQ(d::SignedElapsedUs(kb::Instant{100U}, kb::Instant{40U}), int64_t{60});
  KB_CHECK_EQ(d::SignedElapsedUs(kb::Instant{40U}, kb::Instant{100U}), int64_t{-60});
  KB_CHECK_EQ(d::SampleInstant(kb::Instant{10000U}, 4U, 0U, 1000U).Us(), uint64_t{6000U});
  KB_CHECK_EQ(d::SampleInstant(kb::Instant{10000U}, 4U, 3U, 1000U).Us(), uint64_t{9000U});
  KB_CHECK_EQ(d::SampleInstant(kb::Instant{10000U}, 4U, 4U, 1000U).Us(), uint64_t{10000U});
  KB_CHECK_EQ(d::SampleInstant(kb::Instant{10000U}, 4U, 0U, 0U).Us(), uint64_t{10000U});
  KB_CHECK_EQ(d::SampleInstant(kb::Instant{1000U}, 4U, 0U, 1000U).Us(), uint64_t{0U});
}

static_assert(static_cast<uint8_t>(d::BufferState::Cpu) == 0U);
static_assert(static_cast<uint8_t>(d::BufferState::Device) == 1U);
static_assert(static_cast<uint8_t>(d::BufferState::Quarantined) == 2U);

KB_TEST("buffer: a new buffer belongs to the CPU and every counter is zero") {
  const d::BufferStats empty{};
  d::Buffer<Plain, 10U> b;
  KB_CHECK(b.Current() == d::BufferState::Cpu);
  for (const d::BufferStats& s : {empty, b.Stats()}) {
    KB_CHECK_EQ(s.lends, 0U);
    KB_CHECK_EQ(s.completions, 0U);
    KB_CHECK_EQ(s.errors, 0U);
    KB_CHECK_EQ(s.aborts, 0U);
    KB_CHECK_EQ(s.quarantines, 0U);
    KB_CHECK_EQ(s.dropped, 0U);
    KB_CHECK_EQ(s.recoveries, 0U);
    KB_CHECK_EQ(s.cpu_denied, 0U);
  }
}

static_assert(static_cast<uint8_t>(d::Outcome::Ok) == 0U &&
              static_cast<uint8_t>(d::Outcome::Error) == 1U);
static_assert(static_cast<uint8_t>(d::StopEvidence::NotConfirmed) == 0U &&
              static_cast<uint8_t>(d::StopEvidence::Confirmed) == 1U);
static_assert(static_cast<uint8_t>(d::CancelClaim::Nothing) == 0U &&
              static_cast<uint8_t>(d::CancelClaim::CancelledPending) == 1U &&
              static_cast<uint8_t>(d::CancelClaim::DroppedCompletion) == 2U);
static_assert(static_cast<uint8_t>(d::CachePolicy::Coherent) == 0U &&
              static_cast<uint8_t>(d::CachePolicy::NonCacheable) == 1U &&
              static_cast<uint8_t>(d::CachePolicy::Maintained) == 2U);
static_assert(static_cast<uint8_t>(d::Direction::ToDevice) == 0U &&
              static_cast<uint8_t>(d::Direction::FromDevice) == 1U);
static_assert(static_cast<uint8_t>(d::TransferMode::Polled) == 2U);
static_assert(d::MasterBit(31U) == 0x80000000U && d::MasterBit(32U) == 0U &&
              d::MasterBit(0U) == 1U);
static_assert(d::RoundUp(0U, 32U) == 0U && d::RoundUp(1U, 1U) == 1U && d::RoundUp(31U, 32U) == 32U);
static_assert(d::RoundUp(~std::size_t{0U}, 32U) == (~std::size_t{0U} & ~std::size_t{31U}));
static_assert(d::RoundUp(~std::size_t{0U} & ~std::size_t{31U}, 32U) ==
              (~std::size_t{0U} & ~std::size_t{31U}));
static_assert(d::IsMultipleOf(0U, 32U) && d::IsMultipleOf(32U, 32U) && !d::IsMultipleOf(31U, 32U) &&
              !d::IsMultipleOf(33U, 32U));
[[maybe_unused]] constexpr bool kEdgeHelpersAccept = [] {
  d::RequireFrameFits<128U, 128U>();
  d::RequireWholeUnits<32U, Cached>();
  return true;
}();

KB_TEST("capabilities: defaults of the records") {
  const d::Capabilities c{};
  KB_CHECK(c.mode == d::TransferMode::Polled);
  KB_CHECK(!c.zero_copy);
  KB_CHECK(!c.confirmed_abort);
  KB_CHECK_EQ(c.max_frame_bytes, std::size_t{0U});
  KB_CHECK_EQ(c.staging_bytes, std::size_t{0U});
  KB_CHECK_EQ(c.align_bytes, std::size_t{1U});
  const d::DmaHealth h{};
  KB_CHECK(h.mode == d::TransferMode::Polled);
  KB_CHECK(!h.faulted);
  KB_CHECK_EQ(h.quarantined, 0U);
  KB_CHECK_EQ(h.errors, 0U);
}

KB_TEST("lease: the used length is zero when inactive, after every way a lease ends, and in a "
        "moved-from lease") {
  d::Buffer<Plain, 1U> b;
  d::NoCacheOps ops;
  d::Lease<Plain> fresh;
  KB_CHECK_EQ(fresh.Used(), std::size_t{0U});
  d::Lease<Plain> a;
  KB_CHECK_EQ(b.Lend(Port::Key(), d::Direction::ToDevice, 1U, ops, a), Status::Ok);
  KB_CHECK_EQ(a.Used(), std::size_t{1U});
  d::Lease<Plain> moved{std::move(a)};
  // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  KB_CHECK_EQ(a.Used(), std::size_t{0U});
  KB_CHECK_EQ(moved.Complete(Port::Key(), ops, d::Outcome::Ok), Status::Ok);
  KB_CHECK_EQ(moved.Used(), std::size_t{0U});
  KB_CHECK_EQ(b.Lend(Port::Key(), d::Direction::ToDevice, 1U, ops, a), Status::Ok);
  KB_CHECK_EQ(a.Abort(Port::Key(), ops, d::StopEvidence::NotConfirmed), Status::Fault);
  KB_CHECK_EQ(a.Used(), std::size_t{0U});
  KB_CHECK_EQ(b.Recover(Port::Key(), ops, d::StopEvidence::Confirmed), Status::Ok);
  KB_CHECK_EQ(b.Lend(Port::Key(), d::Direction::ToDevice, 1U, ops, a), Status::Ok);
  KB_CHECK_EQ(a.Abort(Port::Key(), ops, d::StopEvidence::Confirmed), Status::Ok);
  KB_CHECK_EQ(a.Used(), std::size_t{0U});
}

KB_TEST("buffer: Verify at the edges of the region") {
  d::Buffer<Cached, 40U> b;
  const std::uintptr_t at = d::AddressOf(b.Header().Base());
  KB_CHECK_EQ(b.Verify(at, 64U), Status::Ok);
  KB_CHECK_EQ(b.Verify(at, 63U), Status::OutOfRange);
  KB_CHECK_EQ(b.Verify(at - 32U, 96U), Status::Ok);
  KB_CHECK_EQ(b.Verify(at - 32U, 95U), Status::OutOfRange);
  KB_CHECK_EQ(b.Verify(at + 1U, 4096U), Status::OutOfRange);
  KB_CHECK_EQ(b.Verify(at, 0U), Status::OutOfRange);
  KB_CHECK_EQ(b.Verify(at + 32U, 0U), Status::OutOfRange);
}

KB_TEST("buffer: the placement rule at its edges") {
  using B = d::Buffer<Cached, 40U>; // 64 bytes, aligned to 32
  KB_CHECK_EQ(B::CheckPlacement(0x1000U, 0x1000U, 64U), Status::Ok);
  KB_CHECK_EQ(B::CheckPlacement(0x1020U, 0x1000U, 96U), Status::Ok);
  KB_CHECK_EQ(B::CheckPlacement(0x1020U, 0x1000U, 95U), Status::OutOfRange);
  KB_CHECK_EQ(B::CheckPlacement(0x1000U, 0x1000U, 63U), Status::OutOfRange);
  KB_CHECK_EQ(B::CheckPlacement(0x0FE0U, 0x1000U, 4096U), Status::OutOfRange);
  KB_CHECK_EQ(B::CheckPlacement(0x1000U, 0x1000U, 0U), Status::OutOfRange);
  // Alignment is checked on every low bit, whatever the higher bits are.
  KB_CHECK_EQ(B::CheckPlacement(0x1001U, 0x1000U, 4096U), Status::InvalidArgument);
  KB_CHECK_EQ(B::CheckPlacement(0x101FU, 0x1000U, 4096U), Status::InvalidArgument);
  KB_CHECK_EQ(B::CheckPlacement(0x1010U, 0x1000U, 4096U), Status::InvalidArgument);
  KB_CHECK_EQ(B::CheckPlacement(0x1020U, 0x1000U, 4096U), Status::Ok);
  KB_CHECK_EQ(B::CheckPlacement(0x1040U, 0x1000U, 4096U), Status::Ok);
  KB_CHECK_EQ(B::CheckPlacement(0x1060U, 0x1000U, 4096U), Status::Ok);
}
