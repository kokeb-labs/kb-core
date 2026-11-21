/**
 * @file stream.hpp
 * @brief `kb::dma::Stream<Frame, Frames, Region>`: continuous acquisition (ADC scans, a converter
 * streamed over SPI, UART receive) from a port's DMA interrupt to one owner task.
 *
 * One producer (the port's DMA interrupt), one consumer (the owner task), `Frames` slots of
 * kb-owned memory. The interrupt copies each finished hardware frame into the next free slot and
 * never decodes it; the task takes the oldest frame, works on it in place and gives it back. A
 * frame the consumer holds is never overwritten.
 *
 * **Every frame carries** a 32-bit sequence number (counts every hardware frame, including the ones
 * dropped), the completion time read in the interrupt, the hardware flags, `Gap()` (frames or data
 * were lost before it), `Torn()` (the copy may mix two hardware passes), `Fallback()` (it came from
 * the declared fallback producer) and the producer epoch (it changes on a restart or a mode switch,
 * so a consumer resets its filters instead of mixing producers).
 *
 * **Overrun is never data.** Three checks, each counted:
 * 1. the next slot is still held: the new frame is dropped (`overruns`), the next delivered frame
 * has `Gap()`;
 * 2. the hardware lapped the copy (both half- and full-transfer flags pending, or the transfer
 * counter found inside the half just copied): the port publishes with `Integrity::Torn` (`torn`);
 * 3. the peripheral's own overrun flag or a pool overflow: `OnOverrun()` (`hw_overruns`) publishes
 *    nothing and flags the next frame.
 *
 * **Supervision.** `CheckStall` compares the consumer's `now` with the producer's last frame time
 * as a *signed* difference (a frame stamped after the consumer read its clock is not a stall),
 * `Judge` runs the handler's content check, and `Supervise` turns stalls and runs of bad frames
 * into `Restart`, `BackOff` (a restart that recurs inside the back-off window waits) or
 * `UseFallback`.
 *
 * @code
 * kb::dma::Stream<Scan, 4, Sram> stream{kb::dma::StreamConfig{...}};
 * // interrupt (port):  static_cast<void>(stream.Publish(scan, kb::Instant{now_us}, flags));
 * // owner task:
 * Stream::FrameView view;
 * if (stream.TryTake(view) == kb::Status::Ok) { Use(view.Data()); }   // released when view ends
 * @endcode
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_DMA_STREAM_HPP
#define KB_DMA_STREAM_HPP

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

#include "kb/dma/buffer.hpp"
#include "kb/dma/cache.hpp"
#include "kb/dma/region.hpp"
#include "kb/utils/status.hpp"
#include "kb/utils/time.hpp"

namespace kb::dma {

/** @brief Whether the port believes a published frame is a single consistent hardware pass. */
enum class Integrity : uint8_t {
  Clean = 0, ///< One pass: the copy is consistent.
  Torn = 1,  ///< The hardware lapped the copy (both flags pending, counter inside the half).
};

/** @brief What the interrupt knows about one completed frame. */
struct Completion {
  kb::Instant at;                        ///< The completion time, read in the interrupt.
  uint32_t hw_flags{0U};                 ///< The hardware flags the interrupt read (diagnostics).
  Integrity integrity{Integrity::Clean}; ///< `Torn` when the hardware lapped the copy.
};

/** @brief Who produces the stream's frames. */
enum class StreamMode : uint8_t {
  Dma = 0, ///< The declared DMA path.
  Fallback =
      1, ///< The handler's declared fallback producer (polled or one-shot); reported, never silent.
};

/** @brief What the supervisor asks the owner to do next. */
enum class SupervisorAction : uint8_t {
  None = 0,        ///< The stream is healthy (or not running).
  Restart = 1,     ///< Stop, restart the hardware, then call `NoteRestart`.
  BackOff = 2,     ///< A restart is due but the previous one was too recent: wait.
  UseFallback = 3, ///< Restarts keep failing: switch to the fallback producer (`EnterFallback`).
};

/** @brief The tuning of one stream's supervision. */
struct StreamConfig {
  kb::Microseconds frame_period; ///< The expected time between frames; 0 turns the stall check off.
  uint32_t stall_periods{10U};   ///< Frames missing for this many periods is a stall.
  uint32_t bad_frames_to_restart{8U}; ///< This many bad frames in a row ask for a restart; 0 never.
  kb::Milliseconds restart_backoff{1000U}; ///< A second restart inside this window waits.
  uint32_t restarts_before_fallback{
      0U}; ///< This many restarts without a good frame ask for the fallback; 0: none declared.
};

/** @brief Counters and state of one stream. */
struct StreamStats {
  uint32_t published{0U};           ///< Hardware frames reported (the next sequence number).
  uint32_t delivered{0U};           ///< Frames taken by the consumer.
  uint32_t overruns{0U};            ///< Frames dropped because the next slot was held.
  uint32_t gaps{0U};                ///< Delivered frames flagged `Gap()`.
  uint32_t torn{0U};                ///< Frames published `Torn`.
  uint32_t hw_overruns{0U};         ///< `OnOverrun` calls (peripheral overrun, pool overflow).
  uint32_t bad_frames{0U};          ///< Frames the content check rejected.
  uint32_t stalls{0U};              ///< Stall episodes detected.
  uint32_t restarts{0U};            ///< `NoteRestart` calls.
  uint32_t fallbacks{0U};           ///< `EnterFallback` calls.
  uint32_t max_isr_cycles{0U};      ///< The largest interrupt cost reported with `NoteIsrCycles`.
  uint32_t epoch{0U};               ///< The current producer epoch.
  uint32_t last_hw_flags{0U};       ///< The hardware flags of the last overrun or frame.
  StreamMode mode{StreamMode::Dma}; ///< Who produces now.
  bool running{false};              ///< Between `Start` and `Stop`.
};

/**
 * @brief Signed microseconds from `since` to `now` (negative when `since` is later).
 *
 * The 64-bit clock cannot wrap in service, so the modular difference read as signed is exact. Use
 * it wherever two stamps taken in different contexts are compared (the stamp of an interrupt may be
 * later than the task's earlier reading of the clock); an unsigned difference of such a pair reads
 * as a huge elapsed time.
 */
[[nodiscard]] constexpr int64_t SignedElapsedUs(kb::Instant now, kb::Instant since) noexcept {
  return static_cast<int64_t>(now.Us() - since.Us());
}

/**
 * @brief The estimated time of element `index` of a frame of `samples` elements done at `done`,
 * sampled at `rate_hz`: `done - (samples - index) / rate`. An estimate with the interrupt latency
 * as its error; the sequence number, not this time, orders frames.
 */
[[nodiscard]] constexpr kb::Instant SampleInstant(kb::Instant done, uint32_t samples,
                                                  uint32_t index, uint32_t rate_hz) noexcept {
  constexpr uint64_t kUsPerS = 1000000U;
  uint64_t us = done.Us();
  if ((rate_hz != 0U) && (index < samples)) {
    const uint64_t back = (static_cast<uint64_t>(samples - index) * kUsPerS) / rate_hz;
    us = (back > us) ? 0U : (us - back);
  }
  return kb::Instant{us};
}

/** @brief A frame type a stream can carry: trivially copyable and default-constructible. */
template <typename T>
concept StreamFrame = std::is_trivially_copyable_v<T> &&
                      std::is_nothrow_default_constructible_v<T> && std::is_object_v<T>;

/** @brief A content check: `bool check(const Frame&) noexcept`, true for a plausible frame. */
template <typename C, typename Frame>
concept ContentCheck = std::is_nothrow_invocable_r_v<bool, C&, const Frame&>;

/**
 * @brief The stream (see the file note).
 *
 * @tparam Frame  The frame type (`StreamFrame`).
 * @tparam Frames The number of slots, at least 2 (the circular two-half hardware buffer is 2).
 * @tparam R      The region of the hardware frame buffer the port's DMA fills.
 */
template <StreamFrame Frame, std::size_t Frames, Region R>
class Stream {
  static_assert(Frames >= 2U, "kb::dma::Stream needs at least two frames");

public:
  /// The frame type.
  using FrameType = Frame;
  /// The distance between two hardware frames in the DMA buffer: one frame padded to the region's
  /// unit.
  static constexpr std::size_t kFrameStride = RoundUp(sizeof(Frame), R::kAlignBytes);
  /// The size of the hardware buffer that holds `Frames` frames.
  static constexpr std::size_t kHardwareBytes = kFrameStride * Frames;
  /// The hardware buffer type the board defines for this stream.
  using HardwareBuffer = Buffer<R, kHardwareBytes>;

  /** @brief A stream with the default supervision (no stall check). */
  constexpr Stream() noexcept = default;
  /** @brief A stream with `config`. */
  constexpr explicit Stream(const StreamConfig& config) noexcept : config_(config) {}
  Stream(const Stream&) = delete;
  Stream& operator=(const Stream&) = delete;
  Stream(Stream&&) = delete;
  Stream& operator=(Stream&&) = delete;
  ~Stream() = default;

  /** @brief One frame taken by the consumer; the slot is released when the view ends. */
  class FrameView {
  public:
    /** @brief An empty view. */
    constexpr FrameView() noexcept = default;
    FrameView(const FrameView&) = delete;
    FrameView& operator=(const FrameView&) = delete;
    /** @brief Takes over `other`'s frame (releasing the one this view holds). */
    FrameView& operator=(FrameView&& other) noexcept {
      if (this != &other) {
        if (stream_ != nullptr) {
          stream_->release();
        }
        stream_ = other.stream_;
        slot_ = other.slot_;
        seq_ = other.seq_;
        at_ = other.at_;
        hw_flags_ = other.hw_flags_;
        epoch_ = other.epoch_;
        gap_ = other.gap_;
        torn_ = other.torn_;
        fallback_ = other.fallback_;
        other.stream_ = nullptr;
        other.slot_ = nullptr;
      }
      return *this;
    }
    /** @brief Takes over `other`'s frame. */
    FrameView(FrameView&& other) noexcept
        : stream_(other.stream_), slot_(other.slot_), seq_(other.seq_), at_(other.at_),
          hw_flags_(other.hw_flags_), epoch_(other.epoch_), gap_(other.gap_), torn_(other.torn_),
          fallback_(other.fallback_) {
      other.stream_ = nullptr;
      other.slot_ = nullptr;
    }
    /** @brief Releases the slot if the view still holds it. */
    ~FrameView() {
      if (stream_ != nullptr) {
        stream_->release();
      }
    }

    /** @brief True while the view holds a frame. */
    [[nodiscard]] bool Valid() const noexcept {
      return stream_ != nullptr;
    }
    /** @brief The frame; only for a valid view. */
    [[nodiscard]] const Frame& Data() const noexcept {
      return *slot_;
    }
    /** @brief The frame, or null for an empty view. */
    [[nodiscard]] const Frame* Get() const noexcept {
      return slot_;
    }
    /** @brief The sequence number of the hardware frame. */
    [[nodiscard]] uint32_t Seq() const noexcept {
      return seq_;
    }
    /** @brief The completion time read in the interrupt. */
    [[nodiscard]] kb::Instant At() const noexcept {
      return at_;
    }
    /** @brief The hardware flags the port reported. */
    [[nodiscard]] uint32_t HwFlags() const noexcept {
      return hw_flags_;
    }
    /** @brief Frames or data were lost before this one. */
    [[nodiscard]] bool Gap() const noexcept {
      return gap_;
    }
    /** @brief The copy may mix two hardware passes. */
    [[nodiscard]] bool Torn() const noexcept {
      return torn_;
    }
    /** @brief The frame came from the fallback producer. */
    [[nodiscard]] bool Fallback() const noexcept {
      return fallback_;
    }
    /** @brief The producer epoch when the frame was published. */
    [[nodiscard]] uint32_t Epoch() const noexcept {
      return epoch_;
    }

  private:
    friend Stream;

    Stream* stream_{nullptr};
    const Frame* slot_{nullptr};
    uint32_t seq_{0U};
    kb::Instant at_;
    uint32_t hw_flags_{0U};
    uint32_t epoch_{0U};
    bool gap_{false};
    bool torn_{false};
    bool fallback_{false};
  };

  /** @name Producer: the port's DMA interrupt, one context only
   * @{ */

  /**
   * @brief Publishes one finished hardware frame (copy-out).
   *
   * @param frame     The frame, already copied out of the DMA buffer.
   * @param at        The completion time, read in the interrupt.
   * @param hw_flags  The hardware flags the interrupt read (kept for diagnostics).
   * @param integrity `Torn` when the hardware lapped the copy.
   * @return `Ok` (a slot took it) or `Full` (the next slot is held: the frame was dropped and
   * counted).
   */
  [[nodiscard]] kb::Status Publish(const Frame& frame, kb::Instant at, uint32_t hw_flags,
                                   Integrity integrity = Integrity::Clean) noexcept {
    const uint32_t seq = published_.load(std::memory_order_relaxed);
    published_.store(seq + 1U, std::memory_order_relaxed);
    last_hw_flags_.store(hw_flags, std::memory_order_relaxed);
    const uint32_t head = head_.load(std::memory_order_relaxed);
    const uint32_t given = given_.load(std::memory_order_acquire);
    kb::Status status = kb::Status::Full;
    if ((head - given) >= Frames) {
      bump(overruns_);
      pending_gap_ = true;
    } else {
      Slot& slot = slots_[write_index_];
      slot.frame = frame;
      slot.seq = seq;
      slot.at = at;
      slot.hw_flags = hw_flags;
      slot.epoch = epoch_.load(std::memory_order_relaxed);
      slot.gap = pending_gap_;
      slot.torn = (integrity == Integrity::Torn);
      slot.fallback = (mode_.load(std::memory_order_relaxed) == StreamMode::Fallback);
      pending_gap_ = false;
      if (slot.torn) {
        bump(torn_);
      }
      write_index_ = nextIndex(write_index_);
      head_.store(head + 1U, std::memory_order_release);
      status = kb::Status::Ok;
    }
    last_us_.store(at.Us(), std::memory_order_release);
    return status;
  }

  /**
   * @brief Publishes frame `index` of the leased hardware buffer: invalidates its lines (a
   * `Maintained` region), copies it out and calls `Publish`.
   * @return As `Publish`; `InvalidArgument` for an inactive or `ToDevice` lease or an index outside
   * the buffer.
   */
  template <typename Tag, CacheOps Ops>
  [[nodiscard]] kb::Status PublishFromDevice(PortKey<Tag> key, Ops& ops, Lease<R>& lease,
                                             std::size_t index, const Completion& done) noexcept {
    kb::Status status = kb::Status::InvalidArgument;
    const std::span<std::byte> device = lease.DeviceView(key);
    if (lease.Active() && (lease.Dir() == Direction::FromDevice) && (index < Frames) &&
        (device.size() >= kHardwareBytes)) {
      const std::span<std::byte> window = device.subspan(index * kFrameStride, kFrameStride);
      if constexpr (R::kPolicy == CachePolicy::Maintained) {
        ops.Invalidate(window);
      }
      ops.Barrier();
      std::array<std::byte, sizeof(Frame)> raw{};
      static_cast<void>(std::copy_n(window.begin(), sizeof(Frame), raw.begin()));
      status = Publish(std::bit_cast<Frame>(raw), done.at, done.hw_flags, done.integrity);
    }
    return status;
  }

  /**
   * @brief A peripheral overrun or a pool overflow: counted, publishes nothing, flags the next
   * frame. Never a frame.
   */
  void OnOverrun(uint32_t hw_flags) noexcept {
    bump(hw_overruns_);
    last_hw_flags_.store(hw_flags, std::memory_order_relaxed);
    pending_gap_ = true;
  }

  /** @brief Records the interrupt's cost in core cycles (the largest is kept). */
  void NoteIsrCycles(uint32_t cycles) noexcept {
    if (cycles > max_isr_cycles_.load(std::memory_order_relaxed)) {
      max_isr_cycles_.store(cycles, std::memory_order_relaxed);
    }
  }
  /** @} */

  /** @name Consumer: the owner task, one context only
   * @{ */

  /**
   * @brief Takes the oldest unread frame.
   * @param out Receives the view; it holds the slot until it ends or goes to `Give`.
   * @return `Ok`; `Empty` (nothing to take); `Busy` (the previous view is still held).
   */
  [[nodiscard]] kb::Status TryTake(FrameView& out) noexcept {
    kb::Status status = kb::Status::Empty;
    if (held_.load(std::memory_order_relaxed)) {
      status = kb::Status::Busy;
    } else if (taken_ != head_.load(std::memory_order_acquire)) {
      const Slot& slot = slots_[read_index_];
      FrameView view{};
      view.stream_ = this;
      view.slot_ = &slot.frame;
      view.seq_ = slot.seq;
      view.at_ = slot.at;
      view.hw_flags_ = slot.hw_flags;
      view.epoch_ = slot.epoch;
      view.gap_ = slot.gap;
      view.torn_ = slot.torn;
      view.fallback_ = slot.fallback;
      out = std::move(view);
      read_index_ = nextIndex(read_index_);
      ++taken_;
      held_.store(true, std::memory_order_relaxed);
      bump(delivered_);
      if (slot.gap) {
        bump(gaps_);
      }
      stall_reported_ = false;
      status = kb::Status::Ok;
    } else {
      status = kb::Status::Empty;
    }
    return status;
  }

  /** @brief Gives the frame back at once (the same as letting the view end). */
  void Give(FrameView&& view) noexcept {
    const FrameView local{std::move(view)};
    static_cast<void>(local);
  }

  /** @brief Runs the handler's content check on a taken frame and records the verdict. */
  template <ContentCheck<Frame> Check>
  [[nodiscard]] bool Judge(const FrameView& view, Check&& check) noexcept {
    const bool good = view.Valid() && std::forward<Check>(check)(view.Data());
    NoteContent(good);
    return good;
  }

  /** @brief Records a content verdict: a good frame clears the bad run and the restart count. */
  void NoteContent(bool good) noexcept {
    if (good) {
      bad_in_row_ = 0U;
      restarts_since_good_ = 0U;
    } else {
      ++bad_in_row_;
      bump(bad_frames_);
    }
  }

  /** @brief Marks the stream running from `now` (arms the stall clock). Call before enabling the
   * hardware. */
  void Start(kb::Instant now) noexcept {
    last_us_.store(now.Us(), std::memory_order_release);
    running_.store(true, std::memory_order_release);
    stall_reported_ = false;
    bad_in_row_ = 0U;
  }

  /** @brief Marks the stream stopped (the supervisor reports nothing). */
  void Stop() noexcept {
    running_.store(false, std::memory_order_release);
  }

  /**
   * @brief True when no frame arrived for `stall_periods` periods at `now` (signed elapsed time).
   *
   * A frame stamped after `now` was read (the interrupt ran in between) counts as zero elapsed.
   * Each stall episode is counted once.
   */
  [[nodiscard]] bool CheckStall(kb::Instant now) noexcept {
    bool stalled = false;
    if (running_.load(std::memory_order_acquire) && (config_.frame_period.Count() != 0U)) {
      const int64_t elapsed =
          SignedElapsedUs(now, kb::Instant{last_us_.load(std::memory_order_acquire)});
      const uint64_t budget =
          static_cast<uint64_t>(config_.frame_period.Count()) * config_.stall_periods;
      stalled = (elapsed > 0) && std::cmp_greater_equal(elapsed, budget);
    }
    if (stalled && !stall_reported_) {
      stall_reported_ = true;
      bump(stalls_);
    }
    return stalled;
  }

  /** @brief Turns stalls and runs of bad frames into the next step (see `SupervisorAction`). */
  [[nodiscard]] SupervisorAction Supervise(kb::Instant now) noexcept {
    SupervisorAction action = SupervisorAction::None;
    const bool stalled = CheckStall(now);
    const bool bad =
        (config_.bad_frames_to_restart != 0U) && (bad_in_row_ >= config_.bad_frames_to_restart);
    if (running_.load(std::memory_order_acquire) && (stalled || bad)) {
      const bool fallback_due = (config_.restarts_before_fallback != 0U) &&
                                (restarts_since_good_ >= config_.restarts_before_fallback) &&
                                (mode_.load(std::memory_order_relaxed) == StreamMode::Dma);
      if (fallback_due) {
        action = SupervisorAction::UseFallback;
      } else if (restartTooRecent(now)) {
        action = SupervisorAction::BackOff;
      } else {
        action = SupervisorAction::Restart;
      }
    }
    return action;
  }

  /** @brief The owner restarted the hardware: new epoch, stall clock re-armed, bad run cleared. */
  void NoteRestart(kb::Instant now) noexcept {
    restart_at_ = now;
    had_restart_ = true;
    ++restarts_since_good_;
    bump(restarts_);
    rearm(now);
  }

  /** @brief The owner switched to the fallback producer: frames are flagged until `ReturnToDma`. */
  void EnterFallback(kb::Instant now) noexcept {
    mode_.store(StreamMode::Fallback, std::memory_order_release);
    restarts_since_good_ = 0U;
    bump(fallbacks_);
    rearm(now);
  }

  /** @brief The owner switched back to the DMA producer. */
  void ReturnToDma(kb::Instant now) noexcept {
    mode_.store(StreamMode::Dma, std::memory_order_release);
    rearm(now);
  }
  /** @} */

  /** @brief Who produces now. */
  [[nodiscard]] StreamMode Mode() const noexcept {
    return mode_.load(std::memory_order_acquire);
  }

  /** @brief The counters and state. */
  [[nodiscard]] StreamStats Stats() const noexcept {
    StreamStats s{};
    s.published = published_.load(std::memory_order_relaxed);
    s.delivered = delivered_.load(std::memory_order_relaxed);
    s.overruns = overruns_.load(std::memory_order_relaxed);
    s.gaps = gaps_.load(std::memory_order_relaxed);
    s.torn = torn_.load(std::memory_order_relaxed);
    s.hw_overruns = hw_overruns_.load(std::memory_order_relaxed);
    s.bad_frames = bad_frames_.load(std::memory_order_relaxed);
    s.stalls = stalls_.load(std::memory_order_relaxed);
    s.restarts = restarts_.load(std::memory_order_relaxed);
    s.fallbacks = fallbacks_.load(std::memory_order_relaxed);
    s.max_isr_cycles = max_isr_cycles_.load(std::memory_order_relaxed);
    s.epoch = epoch_.load(std::memory_order_relaxed);
    s.last_hw_flags = last_hw_flags_.load(std::memory_order_relaxed);
    s.mode = mode_.load(std::memory_order_relaxed);
    s.running = running_.load(std::memory_order_relaxed);
    return s;
  }

  /** @brief The time of the last published (or dropped) frame, as the producer stamped it. */
  [[nodiscard]] kb::Instant LastFrameAt() const noexcept {
    return kb::Instant{last_us_.load(std::memory_order_acquire)};
  }

private:
  struct Slot {
    Frame frame{};
    uint32_t seq{0U};
    kb::Instant at;
    uint32_t hw_flags{0U};
    uint32_t epoch{0U};
    bool gap{false};
    bool torn{false};
    bool fallback{false};
  };

  static void bump(std::atomic<uint32_t>& counter) noexcept {
    static_cast<void>(counter.fetch_add(1U, std::memory_order_relaxed));
  }

  [[nodiscard]] static constexpr std::size_t nextIndex(std::size_t index) noexcept {
    return ((index + 1U) == Frames) ? 0U : (index + 1U);
  }

  void release() noexcept {
    held_.store(false, std::memory_order_relaxed);
    given_.store(taken_, std::memory_order_release);
  }

  [[nodiscard]] bool restartTooRecent(kb::Instant now) const noexcept {
    bool recent = false;
    if (had_restart_) {
      const int64_t since = SignedElapsedUs(now, restart_at_);
      const uint64_t window = static_cast<uint64_t>(config_.restart_backoff.Count()) * 1000U;
      recent = (since < 0) || std::cmp_less(since, window);
    }
    return recent;
  }

  void rearm(kb::Instant now) noexcept {
    bump(epoch_);
    bad_in_row_ = 0U;
    stall_reported_ = false;
    last_us_.store(now.Us(), std::memory_order_release);
  }

  StreamConfig config_{};
  std::array<Slot, Frames> slots_{};
  // Producer side.
  std::atomic<uint32_t> head_{0U};      // slots published (producer writes)
  std::atomic<uint32_t> published_{0U}; // hardware frames reported = next sequence number
  bool pending_gap_{false};
  std::size_t write_index_{0U}; // the slot the next frame goes to
  // Consumer side.
  std::atomic<uint32_t> given_{0U}; // slots released (consumer writes)
  uint32_t taken_{0U};
  std::size_t read_index_{0U}; // the slot the next take reads
  std::atomic<bool> held_{false};
  uint32_t bad_in_row_{0U};
  uint32_t restarts_since_good_{0U};
  bool stall_reported_{false};
  bool had_restart_{false};
  kb::Instant restart_at_;
  // Shared.
  std::atomic<uint64_t> last_us_{0U};
  std::atomic<bool> running_{false};
  std::atomic<StreamMode> mode_{StreamMode::Dma};
  std::atomic<uint32_t> epoch_{0U};
  std::atomic<uint32_t> last_hw_flags_{0U};
  std::atomic<uint32_t> delivered_{0U};
  std::atomic<uint32_t> overruns_{0U};
  std::atomic<uint32_t> gaps_{0U};
  std::atomic<uint32_t> torn_{0U};
  std::atomic<uint32_t> hw_overruns_{0U};
  std::atomic<uint32_t> bad_frames_{0U};
  std::atomic<uint32_t> stalls_{0U};
  std::atomic<uint32_t> restarts_{0U};
  std::atomic<uint32_t> fallbacks_{0U};
  std::atomic<uint32_t> max_isr_cycles_{0U};
};

} // namespace kb::dma

#endif // KB_DMA_STREAM_HPP
