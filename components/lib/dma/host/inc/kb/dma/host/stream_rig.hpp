/**
 * @file stream_rig.hpp
 * @brief The rig of the stream cases T9 to T13: a circular two-half ADC ring model, the port's
 * interrupt (the only drainer of its flags), and `NaiveStream`, the stream of the earlier
 * implementations with their defects, to run the same cases against.
 *
 * The ring writes `samples-per-half` 16-bit samples per half; sample *k* of the whole run has the
 * value *k*, so a hardware frame number *h* holds `h * 4 .. h * 4 + 3` and a copy that mixes two
 * passes is visible as non-consecutive values. The flag register has half-transfer (HT),
 * transfer-complete (TC) and overrun (OVR) bits, like a DMA stream.
 *
 * Test code, outside the MISRA claim.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_DMA_HOST_STREAM_RIG_HPP
#define KB_DMA_HOST_STREAM_RIG_HPP

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

#include "kb/dma/buffer.hpp"
#include "kb/dma/cache.hpp"
#include "kb/dma/host/machine.hpp"
#include "kb/dma/host/model_port.hpp"
#include "kb/dma/host/rig.hpp"
#include "kb/dma/stream.hpp"
#include "kb/utils/status.hpp"
#include "kb/utils/time.hpp"

namespace kb::dma::host {

/// Four 16-bit samples: one hardware frame (half of the ring).
struct Scan {
  std::array<uint16_t, 4> v{};
};

/// The flag bits of the ring.
inline constexpr uint32_t kRingHalfTransfer = 1U;
inline constexpr uint32_t kRingTransferComplete = 2U;
inline constexpr uint32_t kRingOverrun = 4U;

/** @brief True when a frame holds consecutive values (one hardware pass). */
[[nodiscard]] inline bool ScanConsistent(const Scan& s) noexcept {
  bool ok = true;
  for (std::size_t i = 1U; i < s.v.size(); ++i) {
    ok = ok && (static_cast<uint16_t>(s.v[i - 1U] + 1U) == s.v[i]);
  }
  return ok;
}

/** @brief The behaviours of the earlier stream implementations. */
struct StreamFaults {
  bool silent_overrun{false};   ///< T9: a held frame is overwritten, nothing is counted or flagged.
  bool unsigned_stall{false};   ///< T12: the stall check subtracts unsigned.
  bool overflow_as_data{false}; ///< T13: an overflow reaches the consumer as an empty frame.
};

/**
 * @brief The stream of the earlier implementations: a ring that the producer overwrites whenever it
 * likes, with the stall check and the overflow handling of the incidents (see `StreamFaults`). The
 * same member functions as `kb::dma::Stream`, so the cases run on both.
 */
template <typename Frame, std::size_t Frames>
class NaiveStream {
public:
  /** @brief A view of a slot (not owned: the slot may change under it). */
  class FrameView {
  public:
    [[nodiscard]] bool Valid() const noexcept {
      return slot_ != nullptr;
    }
    [[nodiscard]] const Frame& Data() const noexcept {
      return *slot_;
    }
    [[nodiscard]] uint32_t Seq() const noexcept {
      return seq_;
    }
    [[nodiscard]] bool Gap() const noexcept {
      return gap_;
    }
    [[nodiscard]] bool Torn() const noexcept {
      return torn_;
    }

  private:
    friend class NaiveStream;
    const Frame* slot_{nullptr};
    uint32_t seq_{0U};
    bool gap_{false};
    bool torn_{false};
  };

  /** @brief A naive stream with `config` and `faults`. */
  NaiveStream(const kb::dma::StreamConfig& config, const StreamFaults& faults) noexcept
      : config_(config), faults_(faults) {}

  void Start(kb::Instant now) noexcept {
    last_ = now;
    running_ = true;
  }

  [[nodiscard]] kb::Status Publish(
      const Frame& frame, kb::Instant at, uint32_t /*hw_flags*/,
      kb::dma::Integrity integrity = kb::dma::Integrity::Clean) noexcept {
    slots_[head_ % Frames] = frame;
    seqs_[head_ % Frames] = published_;
    torn_[head_ % Frames] = (integrity == kb::dma::Integrity::Torn);
    ++head_;
    ++published_;
    last_ = at;
    return kb::Status::Ok;
  }

  void OnOverrun(uint32_t hw_flags) noexcept {
    if (faults_.overflow_as_data) {
      static_cast<void>(Publish(Frame{}, last_, hw_flags));
    } else {
      ++hw_overruns_;
    }
  }

  [[nodiscard]] kb::Status TryTake(FrameView& out) noexcept {
    kb::Status status = kb::Status::Empty;
    if (taken_ != head_) {
      const std::size_t index = taken_ % Frames;
      out.slot_ = &slots_[index];
      out.seq_ = seqs_[index];
      out.torn_ = torn_[index];
      out.gap_ = false;
      ++taken_;
      status = kb::Status::Ok;
    }
    return status;
  }

  void Give(FrameView&& view) noexcept {
    const FrameView sink = std::move(view);
    static_cast<void>(sink);
  }

  [[nodiscard]] bool CheckStall(kb::Instant now) noexcept {
    bool stalled = false;
    if (running_ && (config_.frame_period.Count() != 0U)) {
      const uint64_t budget =
          static_cast<uint64_t>(config_.frame_period.Count()) * config_.stall_periods;
      if (faults_.unsigned_stall) {
        stalled = (now.Us() - last_.Us()) >= budget;
      } else {
        const int64_t elapsed = kb::dma::SignedElapsedUs(now, last_);
        stalled = (elapsed > 0) && std::cmp_greater_equal(elapsed, budget);
      }
    }
    return stalled;
  }

  [[nodiscard]] kb::dma::StreamStats Stats() const noexcept {
    kb::dma::StreamStats s{};
    s.published = published_;
    s.delivered = taken_;
    s.hw_overruns = hw_overruns_;
    return s;
  }

private:
  kb::dma::StreamConfig config_;
  StreamFaults faults_;
  std::array<Frame, Frames> slots_{};
  std::array<uint32_t, Frames> seqs_{};
  std::array<bool, Frames> torn_{};
  uint32_t head_{0U};
  uint32_t taken_{0U};
  uint32_t published_{0U};
  uint32_t hw_overruns_{0U};
  kb::Instant last_;
  bool running_{false};
};

/** @brief The circular ring hardware: two halves of four samples. */
class HwRing {
public:
  /// Samples per half.
  static constexpr std::size_t kSamples = 4U;

  /** @brief Points the ring at its DMA memory (16 bytes). */
  void Attach(Machine& machine, std::byte* memory) noexcept {
    machine_ = &machine;
    memory_ = memory;
    pos_ = 0U;
    counter_ = 0U;
    halves_ = 0U;
    flags_ = 0U;
  }

  /** @brief The hardware writes `n` samples. */
  void Advance(std::size_t n) noexcept {
    for (std::size_t i = 0U; i < n; ++i) {
      const auto value = static_cast<uint16_t>(counter_);
      const std::array<std::byte, 2> bytes{static_cast<std::byte>(value & 0xFFU),
                                           static_cast<std::byte>(value >> 8U)};
      machine_->DmaWrite(memory_ + (pos_ * 2U), bytes);
      ++counter_;
      ++pos_;
      if (pos_ == kSamples) {
        flags_ |= kRingHalfTransfer;
        ++halves_;
      } else if (pos_ == (2U * kSamples)) {
        flags_ |= kRingTransferComplete;
        ++halves_;
        pos_ = 0U;
      } else {
        // Inside a half: nothing to flag.
      }
    }
  }

  /** @brief The peripheral overrun flag is raised. */
  void RaiseOverrun() noexcept {
    flags_ |= kRingOverrun;
  }

  [[nodiscard]] uint32_t Flags() const noexcept {
    return flags_;
  }
  void ClearFlags(uint32_t mask) noexcept {
    flags_ &= ~mask;
  }
  /** @brief The write position in samples (the transfer counter). */
  [[nodiscard]] std::size_t Pos() const noexcept {
    return pos_;
  }
  /** @brief Halves the hardware has completed. */
  [[nodiscard]] uint32_t Halves() const noexcept {
    return halves_;
  }
  [[nodiscard]] std::byte* Memory() const noexcept {
    return memory_;
  }

private:
  Machine* machine_{nullptr};
  std::byte* memory_{nullptr};
  std::size_t pos_{0U};
  uint32_t counter_{0U};
  uint32_t halves_{0U};
  uint32_t flags_{0U};
};

/**
 * @brief The stream rig over a stream type `S` (`kb::dma::Stream` or `NaiveStream`): the ring, the
 * port's interrupt and the owner-task helpers.
 */
template <typename S>
class AdcRig {
public:
  /// The ring's DMA buffer: two halves in an uncached region.
  using HwBuffer = kb::dma::Buffer<ModelUncached, 2U * sizeof(Scan)>;

  /** @brief A rig that publishes into `stream` with the defects in `faults`. */
  AdcRig(S& stream, const Faults& faults) noexcept
      : stream_(stream), faults_(faults), machine_(1U, false) {
    static_cast<void>(machine_.RegisterBuffer(hw_.RawMemory(Key()), hw_.Header(), false));
  }

  /** @brief The key of this port. */
  [[nodiscard]] kb::dma::PortKey<AdcRig> Key() const noexcept {
    return kb::dma::PortKey<AdcRig>{};
  }

  /** @brief Lends the ring buffer to the hardware and starts the stream clock at `now`. */
  [[nodiscard]] kb::Status Start(kb::Instant now) noexcept {
    const kb::Status status =
        hw_.Lend(Key(), kb::dma::Direction::FromDevice, HwBuffer::kBytes, ops_, lease_);
    if (status == kb::Status::Ok) {
      ring_.Attach(machine_, lease_.DeviceView(Key()).data());
      stream_.Start(now);
    }
    return status;
  }

  /** @brief The hardware writes `n` samples (no interrupt). */
  void Advance(std::size_t n) noexcept {
    ring_.Advance(n);
  }

  /** @brief The hardware completes `halves` halves, the interrupt serving each on time at `at`. */
  void RunHalves(std::size_t halves, kb::Instant at) noexcept {
    for (std::size_t i = 0U; i < halves; ++i) {
      ring_.Advance(HwRing::kSamples);
      Isr(at);
    }
  }

  /** @brief The port's interrupt: the only code that reads and clears the flags. */
  void Isr(kb::Instant at) noexcept {
    const uint32_t flags = ring_.Flags();
    uint32_t handled = 0U;
    if ((flags & kRingOverrun) != 0U) {
      stream_.OnOverrun(flags);
      handled |= kRingOverrun;
    }
    const bool both =
        ((flags & kRingHalfTransfer) != 0U) && ((flags & kRingTransferComplete) != 0U);
    if ((flags & kRingHalfTransfer) != 0U) {
      publishHalf(0U, at, flags, both);
      handled |= kRingHalfTransfer;
    }
    if ((flags & kRingTransferComplete) != 0U) {
      publishHalf(1U, at, flags, false);
      handled |= kRingTransferComplete;
    }
    ring_.ClearFlags(handled);
  }

  /**
   * @brief A task that also drains the flags (the earlier implementation's second drainer): it
   * reads the flags, the interrupt preempts it and serves them, then it publishes and clears what
   * it read. A correct rig has no task drain; this does nothing.
   */
  void TaskDrainPreempted(kb::Instant at) noexcept {
    if (faults_.two_drainers) {
      const uint32_t seen = ring_.Flags();
      Isr(at);
      if ((seen & kRingHalfTransfer) != 0U) {
        publishHalf(0U, at, seen, false);
      }
      if ((seen & kRingTransferComplete) != 0U) {
        publishHalf(1U, at, seen, false);
      }
      ring_.ClearFlags(seen);
    }
  }

  /** @brief The peripheral overrun flag. */
  void RaiseOverrun() noexcept {
    ring_.RaiseOverrun();
  }
  /** @brief Hardware halves completed so far. */
  [[nodiscard]] uint32_t HalvesDone() const noexcept {
    return ring_.Halves();
  }
  [[nodiscard]] S& StreamRef() noexcept {
    return stream_;
  }

private:
  void publishHalf(std::size_t half, kb::Instant at, uint32_t flags, bool lapped) noexcept {
    std::array<std::byte, sizeof(Scan)> raw{};
    machine_.PortLoad(ring_.Memory() + (half * sizeof(Scan)), raw);
    bool torn = lapped;
    const std::size_t start = half * HwRing::kSamples;
    if ((ring_.Pos() > start) && (ring_.Pos() < (start + HwRing::kSamples))) {
      torn = true;
    }
    if (faults_.ignore_lapped) {
      torn = false;
    }
    static_cast<void>(stream_.Publish(std::bit_cast<Scan>(raw), at, flags,
                                      torn ? kb::dma::Integrity::Torn : kb::dma::Integrity::Clean));
  }

  S& stream_;
  Faults faults_;
  Machine machine_;
  kb::dma::NoCacheOps ops_{};
  HwBuffer hw_;
  kb::dma::Lease<ModelUncached> lease_;
  HwRing ring_{};
};

} // namespace kb::dma::host

#endif // KB_DMA_HOST_STREAM_RIG_HPP
