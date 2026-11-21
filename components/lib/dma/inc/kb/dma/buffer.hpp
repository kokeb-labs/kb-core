/**
 * @file buffer.hpp
 * @brief `kb::dma::Buffer`, the static, line-padded DMA buffer, and `Lease`, the move-only
 * ownership of it while a transfer runs.
 *
 * **Placement and size.** A `Buffer<Region, Bytes>` is defined once, at static storage duration
 * (the board's generated file puts it in the region's linker section). Its storage is padded to
 * whole alignment units of the region and aligned to the unit, and the buffer's bookkeeping sits in
 * its own units after the data, so no other object shares a cache line with the data and an
 * invalidate can never destroy a neighbour.
 *
 * **Ownership.** A buffer is in one of three states:
 *
 * ```
 *   Cpu --Lend--> Device --Complete / confirmed Abort--> Cpu
 *                   |
 *                   +--unconfirmed Abort / lease destroyed--> Quarantined --Recover (proof)--> Cpu
 * ```
 *
 * While the state is not `Cpu` the CPU has no view: `CpuView()` returns an empty span and counts
 * the attempt. Destroying a lease is **not** a release: a lease that goes out of scope while the
 * hardware may still write quarantines the buffer. Memory returns to the CPU only through the
 * port's `Complete` or an `Abort` with `StopEvidence::Confirmed`.
 *
 * **Cache maintenance** is done here, at the transitions, through the port's `CacheOps`:
 *
 * | Transition | Coherent / NonCacheable | Maintained |
 * | --- | --- | --- |
 * | Lend to the device (`ToDevice`) | barrier | clean the used lines, barrier |
 * | Lend from the device (`FromDevice`) | barrier | clean, then invalidate the whole capacity,
 * barrier | | Complete `FromDevice` (any outcome) | barrier | invalidate the used lines, barrier |
 * | Complete `ToDevice` | barrier | barrier |
 * | Confirmed abort `FromDevice` | barrier | invalidate the whole capacity, barrier |
 * | Recover | barrier | invalidate the whole capacity, barrier |
 *
 * A transfer done by CPU copies (no lease) does no maintenance at all.
 *
 * Only a port can lend, complete, abort or recover: those calls take a `PortKey<Port>`, which only
 * the class `Port` can make (`friend Port`).
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_DMA_BUFFER_HPP
#define KB_DMA_BUFFER_HPP

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

#include "kb/dma/cache.hpp"
#include "kb/dma/completion.hpp"
#include "kb/dma/region.hpp"
#include "kb/utils/status.hpp"

namespace kb::dma {

/** @brief Who may touch a buffer's memory. */
enum class BufferState : uint8_t {
  Cpu = 0,    ///< The CPU owns it; `CpuView()` works.
  Device = 1, ///< A lease is out; a DMA master may be writing or reading.
  Quarantined =
      2, ///< A stop could not be confirmed (or a lease was destroyed): nobody may touch it.
};

/** @brief Counters of one buffer (read with `Buffer::Stats()`). */
struct BufferStats {
  uint32_t lends{0U};       ///< Successful `Lend` calls.
  uint32_t completions{0U}; ///< Leases ended by `Complete(Outcome::Ok)`.
  uint32_t errors{0U};      ///< Leases ended by `Complete(Outcome::Error)`.
  uint32_t aborts{0U};      ///< Leases ended by a confirmed abort.
  uint32_t quarantines{0U}; ///< Times the buffer was quarantined.
  uint32_t dropped{0U};     ///< Leases destroyed while active (a subset of the quarantines).
  uint32_t recoveries{0U};  ///< Times `Recover` returned a quarantined buffer to the CPU.
  uint32_t cpu_denied{0U};  ///< `CpuView()` calls refused because the CPU did not own the buffer.
};

/** @brief The address of `ptr` as an integer (for placement checks and port-side registries). */
[[nodiscard]] inline std::uintptr_t AddressOf(const std::byte* ptr) noexcept {
  return std::bit_cast<std::uintptr_t>(ptr);
}

/**
 * @brief The bookkeeping every buffer carries, readable by ports and registries.
 *
 * Only `Buffer` and `Lease` change it. The class is public so that a port (or the host model) can
 * hold a pointer to it and watch `Current()`.
 */
class BufferHeader {
public:
  /** @brief A header for `capacity` bytes at `base`. */
  constexpr BufferHeader(std::byte* base, std::size_t capacity) noexcept
      : base_(base), capacity_(capacity) {}

  /** @brief Who owns the memory now. */
  [[nodiscard]] BufferState Current() const noexcept {
    return state_.load(std::memory_order_acquire);
  }
  /** @brief The first byte of the data (a placement-check input, not an access path). */
  [[nodiscard]] const std::byte* Base() const noexcept {
    return base_;
  }
  /** @brief The padded size of the data in bytes. */
  [[nodiscard]] std::size_t Capacity() const noexcept {
    return capacity_;
  }
  /** @brief The counters. */
  [[nodiscard]] BufferStats Stats() const noexcept {
    BufferStats s{};
    s.lends = lends_.load(std::memory_order_relaxed);
    s.completions = completions_.load(std::memory_order_relaxed);
    s.errors = errors_.load(std::memory_order_relaxed);
    s.aborts = aborts_.load(std::memory_order_relaxed);
    s.quarantines = quarantines_.load(std::memory_order_relaxed);
    s.dropped = dropped_.load(std::memory_order_relaxed);
    s.recoveries = recoveries_.load(std::memory_order_relaxed);
    s.cpu_denied = cpu_denied_.load(std::memory_order_relaxed);
    return s;
  }

private:
  template <Region R, std::size_t Bytes>
  friend class Buffer;
  template <Region R>
  friend class Lease;

  static void bump(std::atomic<uint32_t>& counter) noexcept {
    static_cast<void>(counter.fetch_add(1U, std::memory_order_relaxed));
  }

  std::byte* base_;
  std::size_t capacity_;
  std::atomic<BufferState> state_{BufferState::Cpu};
  std::atomic<uint32_t> lends_{0U};
  std::atomic<uint32_t> completions_{0U};
  std::atomic<uint32_t> errors_{0U};
  std::atomic<uint32_t> aborts_{0U};
  std::atomic<uint32_t> quarantines_{0U};
  std::atomic<uint32_t> dropped_{0U};
  std::atomic<uint32_t> recoveries_{0U};
  std::atomic<uint32_t> cpu_denied_{0U};
};

/**
 * @brief The proof that a call comes from port class `Tag`: only `Tag` can make one.
 *
 * ```
 * class MyPort {
 *   kb::dma::PortKey<MyPort> Key() const noexcept { return kb::dma::PortKey<MyPort>{}; }
 * };
 * ```
 */
template <typename Tag>
class PortKey {
public:
  PortKey(const PortKey&) noexcept = default;
  PortKey& operator=(const PortKey&) noexcept = default;
  ~PortKey() = default;

private:
  friend Tag;
  constexpr PortKey() noexcept = default;
};

namespace detail {

/// Maintenance at a lend (see the table in buffer.hpp).
template <Region R, CacheOps Ops>
void MaintainOnLend(Direction dir, std::byte* base, std::size_t used, std::size_t capacity,
                    Ops& ops) noexcept {
  if constexpr (R::kPolicy == CachePolicy::Maintained) {
    if (dir == Direction::ToDevice) {
      ops.Clean(std::span<const std::byte>(base, RoundUp(used, R::kLineBytes)));
    } else {
      ops.Clean(std::span<const std::byte>(base, capacity));
      ops.Invalidate(std::span<std::byte>(base, capacity));
    }
  } else {
    static_cast<void>(dir);
    static_cast<void>(base);
    static_cast<void>(used);
    static_cast<void>(capacity);
  }
  ops.Barrier();
}

/// Maintenance at a completion (any outcome) or a confirmed abort.
template <Region R, CacheOps Ops>
void MaintainOnReturn(Direction dir, std::byte* base, std::size_t span_bytes, Ops& ops) noexcept {
  if constexpr (R::kPolicy == CachePolicy::Maintained) {
    if (dir == Direction::FromDevice) {
      ops.Invalidate(std::span<std::byte>(base, span_bytes));
    }
  } else {
    static_cast<void>(dir);
    static_cast<void>(base);
    static_cast<void>(span_bytes);
  }
  ops.Barrier();
}

} // namespace detail

/**
 * @brief The ownership of a buffer while a transfer runs (move-only, made by `Buffer::Lend`).
 *
 * A default-constructed lease is inactive. A lease has no data access at all: the port gets the
 * memory through `DeviceView`, the CPU gets nothing until the lease ends.
 *
 * @tparam R The buffer's region.
 */
template <Region R>
class [[nodiscard]] Lease {
public:
  /** @brief An inactive lease (holds nothing). */
  constexpr Lease() noexcept = default;
  Lease(const Lease&) = delete;
  Lease& operator=(const Lease&) = delete;
  Lease& operator=(Lease&&) = delete;
  /** @brief Takes over `other`'s lend; `other` becomes inactive. */
  Lease(Lease&& other) noexcept : header_(other.header_), dir_(other.dir_), used_(other.used_) {
    other.header_ = nullptr;
    other.used_ = 0U;
  }
  /**
   * @brief Destroying an active lease quarantines the buffer (the hardware may still own it).
   */
  ~Lease() {
    if (header_ != nullptr) {
      header_->state_.store(BufferState::Quarantined, std::memory_order_release);
      BufferHeader::bump(header_->quarantines_);
      BufferHeader::bump(header_->dropped_);
    }
  }

  /** @brief True while the lease holds a buffer. */
  [[nodiscard]] bool Active() const noexcept {
    return header_ != nullptr;
  }
  /** @brief The direction of the transfer (`ToDevice` for an inactive lease). */
  [[nodiscard]] Direction Dir() const noexcept {
    return dir_;
  }
  /** @brief The bytes of the buffer the transfer uses. */
  [[nodiscard]] std::size_t Used() const noexcept {
    return used_;
  }

  /**
   * @brief The whole padded memory of the leased buffer, for the port to program into the DMA.
   * @return The span (the capacity, including the padding), or empty for an inactive lease.
   */
  template <typename Tag>
  [[nodiscard]] std::span<std::byte> DeviceView(PortKey<Tag> key) noexcept {
    static_cast<void>(key);
    return (header_ != nullptr) ? std::span<std::byte>(header_->base_, header_->capacity_)
                                : std::span<std::byte>{};
  }

  /**
   * @brief The transfer ended: maintenance, then the memory goes back to the CPU.
   *
   * Call it only when the port knows the master has stopped (the completion interrupt of a finished
   * transfer or of an error that disabled the stream). After it the lease is inactive.
   *
   * @return `Ok`; `InvalidArgument` for an inactive lease.
   */
  template <typename Tag, CacheOps Ops>
  [[nodiscard]] kb::Status Complete(PortKey<Tag> key, Ops& ops, Outcome outcome) noexcept {
    static_cast<void>(key);
    kb::Status status = kb::Status::InvalidArgument;
    if (header_ != nullptr) {
      BufferHeader* const header = header_;
      detail::MaintainOnReturn<R>(dir_, header->base_, RoundUp(used_, R::kLineBytes), ops);
      header->state_.store(BufferState::Cpu, std::memory_order_release);
      BufferHeader::bump((outcome == Outcome::Ok) ? header->completions_ : header->errors_);
      header_ = nullptr;
      used_ = 0U;
      status = kb::Status::Ok;
    }
    return status;
  }

  /**
   * @brief Ends the transfer early.
   *
   * With `StopEvidence::Confirmed` the memory returns to the CPU and the result is `Ok`. Without it
   * the buffer is quarantined and the result is `Fault`: the memory is not given back while a
   * master may still write it.
   *
   * @return `Ok` (confirmed), `Fault` (quarantined), `InvalidArgument` for an inactive lease.
   */
  template <typename Tag, CacheOps Ops>
  [[nodiscard]] kb::Status Abort(PortKey<Tag> key, Ops& ops, StopEvidence evidence) noexcept {
    static_cast<void>(key);
    kb::Status status = kb::Status::InvalidArgument;
    if (header_ != nullptr) {
      BufferHeader* const header = header_;
      if (evidence == StopEvidence::Confirmed) {
        detail::MaintainOnReturn<R>(dir_, header->base_, header->capacity_, ops);
        header->state_.store(BufferState::Cpu, std::memory_order_release);
        BufferHeader::bump(header->aborts_);
        status = kb::Status::Ok;
      } else {
        header->state_.store(BufferState::Quarantined, std::memory_order_release);
        BufferHeader::bump(header->quarantines_);
        status = kb::Status::Fault;
      }
      header_ = nullptr;
      used_ = 0U;
    }
    return status;
  }

private:
  template <Region R2, std::size_t Bytes>
  friend class Buffer;

  BufferHeader* header_{nullptr};
  Direction dir_{Direction::ToDevice};
  std::size_t used_{0U};
};

/**
 * @brief A static DMA buffer of `Bytes` bytes in region `R`, padded to whole units and aligned.
 *
 * Never copied or moved; define it at namespace scope (constant-initialised, no constructor runs).
 *
 * @tparam R     The region tag (`Region`).
 * @tparam Bytes The usable size, at least 1.
 */
template <Region R, std::size_t Bytes>
class Buffer {
public:
  /// The region this buffer lives in.
  using RegionType = R;
  /// The usable size requested.
  static constexpr std::size_t kBytes = Bytes;
  /// The padded size: a whole number of alignment units.
  static constexpr std::size_t kCapacity = RoundUp(Bytes, R::kAlignBytes);
  /// The alignment of the data.
  static constexpr std::size_t kAlign = R::kAlignBytes;

  static_assert(Bytes > 0U, "a DMA buffer holds at least one byte");
  static_assert((kCapacity % R::kLineBytes) == 0U, "the capacity is a whole number of cache lines");
  static_assert(kCapacity >= Bytes, "the padding never shrinks the buffer");

  constexpr Buffer() noexcept : header_(bytes_.data(), kCapacity) {}
  Buffer(const Buffer&) = delete;
  Buffer& operator=(const Buffer&) = delete;
  Buffer(Buffer&&) = delete;
  Buffer& operator=(Buffer&&) = delete;
  ~Buffer() = default;

  /**
   * @brief The CPU's view of the data (`Bytes` bytes), or an empty span when the CPU does not own
   * the buffer (the attempt is counted in `Stats().cpu_denied`).
   */
  [[nodiscard]] std::span<std::byte> CpuView() noexcept {
    std::span<std::byte> view{};
    if (header_.state_.load(std::memory_order_acquire) == BufferState::Cpu) {
      view = std::span<std::byte>(bytes_.data(), Bytes);
    } else {
      BufferHeader::bump(header_.cpu_denied_);
    }
    return view;
  }

  /** @brief The CPU's read-only view, or empty (not counted). */
  [[nodiscard]] std::span<const std::byte> CpuView() const noexcept {
    return (header_.state_.load(std::memory_order_acquire) == BufferState::Cpu)
               ? std::span<const std::byte>(bytes_.data(), Bytes)
               : std::span<const std::byte>{};
  }

  /** @brief Who owns the memory now. */
  [[nodiscard]] BufferState Current() const noexcept {
    return header_.Current();
  }

  /**
   * @brief The whole padded memory whatever the state (port only): for a registry or a test model
   * that must know where the buffer is. It is not an access path for the CPU's data.
   */
  template <typename Tag>
  [[nodiscard]] std::span<std::byte> RawMemory(PortKey<Tag> key) noexcept {
    static_cast<void>(key);
    return std::span<std::byte>(bytes_.data(), kCapacity);
  }

  /** @brief The counters. */
  [[nodiscard]] BufferStats Stats() const noexcept {
    return header_.Stats();
  }

  /** @brief The bookkeeping (for registries and placement checks). */
  [[nodiscard]] const BufferHeader& Header() const noexcept {
    return header_;
  }

  /**
   * @brief Lends the buffer for a transfer of `used` bytes (port only).
   *
   * Maintenance per the table in the file note runs first; then the state becomes `Device`.
   *
   * @param key  The port's key.
   * @param dir  The direction of the transfer.
   * @param used Bytes the transfer moves, 1 to `Bytes`.
   * @param ops  The port's cache hooks.
   * @param out  Receives the lease; it must be inactive.
   * @return `Ok`; `InvalidArgument` (bad `used`, or `out` is active); `Busy` (a lease is out);
   * `Fault` (quarantined).
   */
  template <typename Tag, CacheOps Ops>
  [[nodiscard]] kb::Status Lend(PortKey<Tag> key, Direction dir, std::size_t used, Ops& ops,
                                Lease<R>& out) noexcept {
    static_cast<void>(key);
    kb::Status status = kb::Status::Ok;
    const BufferState state = header_.state_.load(std::memory_order_acquire);
    if (out.Active() || (used == 0U) || (used > Bytes)) {
      status = kb::Status::InvalidArgument;
    } else if (state == BufferState::Device) {
      status = kb::Status::Busy;
    } else if (state == BufferState::Quarantined) {
      status = kb::Status::Fault;
    } else {
      detail::MaintainOnLend<R>(dir, bytes_.data(), used, kCapacity, ops);
      header_.state_.store(BufferState::Device, std::memory_order_release);
      BufferHeader::bump(header_.lends_);
      out.header_ = &header_;
      out.dir_ = dir;
      out.used_ = used;
    }
    return status;
  }

  /**
   * @brief Returns a quarantined buffer to the CPU after the port re-initialised the peripheral and
   * proved the master stopped (port only).
   * @return `Ok`; `Fault` (no proof: stays quarantined); `Busy` (a lease is out);
   * `InvalidArgument` (the CPU already owns it).
   */
  template <typename Tag, CacheOps Ops>
  [[nodiscard]] kb::Status Recover(PortKey<Tag> key, Ops& ops, StopEvidence evidence) noexcept {
    static_cast<void>(key);
    kb::Status status = kb::Status::InvalidArgument;
    const BufferState state = header_.state_.load(std::memory_order_acquire);
    if (state == BufferState::Device) {
      status = kb::Status::Busy;
    } else if (state == BufferState::Quarantined) {
      if (evidence == StopEvidence::Confirmed) {
        detail::MaintainOnReturn<R>(Direction::FromDevice, bytes_.data(), kCapacity, ops);
        header_.state_.store(BufferState::Cpu, std::memory_order_release);
        BufferHeader::bump(header_.recoveries_);
        status = kb::Status::Ok;
      } else {
        status = kb::Status::Fault;
      }
    } else {
      status = kb::Status::InvalidArgument;
    }
    return status;
  }

  /**
   * @brief Start-time placement check: the data is aligned to the region's unit and lies inside
   * [`region_base`, `region_base + region_size`).
   * @return `Ok`, `InvalidArgument` (misaligned) or `OutOfRange` (outside the region).
   */
  [[nodiscard]] kb::Status Verify(std::uintptr_t region_base,
                                  std::size_t region_size) const noexcept {
    return CheckPlacement(AddressOf(bytes_.data()), region_base, region_size);
  }

  /**
   * @brief The placement rule of `Verify` for any address (so the rule is testable at its edges):
   * the address is aligned to the region's unit, and the padded buffer lies inside the region.
   */
  [[nodiscard]] static kb::Status CheckPlacement(std::uintptr_t address, std::uintptr_t region_base,
                                                 std::size_t region_size) noexcept {
    kb::Status status = kb::Status::Ok;
    if ((address & (static_cast<std::uintptr_t>(kAlign) - 1U)) != 0U) {
      status = kb::Status::InvalidArgument;
    } else if ((address < region_base) || (kCapacity > region_size) ||
               (static_cast<std::size_t>(address - region_base) > (region_size - kCapacity))) {
      status = kb::Status::OutOfRange;
    } else {
      status = kb::Status::Ok;
    }
    return status;
  }

private:
  alignas(R::kAlignBytes) std::array<std::byte, kCapacity> bytes_{};
  alignas(std::max(static_cast<std::size_t>(R::kAlignBytes),
                   alignof(BufferHeader))) BufferHeader header_;
};

} // namespace kb::dma

#endif // KB_DMA_BUFFER_HPP
