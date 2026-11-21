/**
 * @file machine.hpp
 * @brief The host DMA-and-cache model: memory, a write-back data cache, the CPU's loads and stores,
 * DMA reads and writes, cache maintenance, and a log of every coherency or ownership violation.
 *
 * "Memory" is the real storage of the buffers and arenas registered with the machine. The CPU side
 * (`CpuLoad`, `CpuStore`) goes through a cache model (allocate on read and write, write-back, the
 * line size of the machine); DMA reads and writes (`DmaRead`, `DmaWrite`) go to memory only. Cache
 * maintenance (`CleanRange`, `InvalidateRange`) acts on whole lines. Whenever the two views
 * disagree in a way real hardware would turn into a defect, the machine records a `Violation`:
 *
 * | Violation | Real-world defect |
 * | --- | --- |
 * | `MissingClean` | a DMA read of memory that a dirty CPU line still holds back |
 * | `DirtyUnderDmaWrite` | a DMA write onto a dirty line: a later eviction overwrites the received
 * data | | `StaleCpuRead` | the CPU hit a line that differs from memory: a missing invalidate | |
 * `LostWrite` | an invalidate discarded data the CPU wrote and nobody cleaned | |
 * `UnalignedMaintenance` | a maintenance range that is not whole lines reaches a neighbour's line |
 * | `AccessDuringLease` | a CPU access to a buffer a lease holds |
 * | `DmaAfterReturn` | a DMA access to a buffer the CPU owns again |
 * | `DmaOutsideBuffer` | a DMA access outside every window the DMA owns |
 *
 * Test code, outside the MISRA claim.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_DMA_HOST_MACHINE_HPP
#define KB_DMA_HOST_MACHINE_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "kb/dma/buffer.hpp"

namespace kb::dma::host {

/** @brief The kinds of violation the machine records (see the file note). */
enum class Violation : uint8_t {
  MissingClean = 0,
  DirtyUnderDmaWrite = 1,
  StaleCpuRead = 2,
  LostWrite = 3,
  UnalignedMaintenance = 4,
  AccessDuringLease = 5,
  DmaAfterReturn = 6,
  DmaOutsideBuffer = 7,
};

/** @brief The number of violation kinds. */
inline constexpr std::size_t kViolationKinds = 8U;

/** @brief The name of a violation, for failure messages. */
[[nodiscard]] constexpr const char* ToString(Violation v) noexcept {
  constexpr std::array<const char*, kViolationKinds> kNames{
      "MissingClean",         "DirtyUnderDmaWrite", "StaleCpuRead",   "LostWrite",
      "UnalignedMaintenance", "AccessDuringLease",  "DmaAfterReturn", "DmaOutsideBuffer"};
  const auto index = static_cast<std::size_t>(v);
  return (index < kNames.size()) ? kNames[index] : "Unknown";
}

/** @brief Counters of the maintenance calls the machine received. */
struct MaintenanceCounts {
  uint32_t cleans{0U};
  uint32_t invalidates{0U};
  uint32_t barriers{0U};
};

/** @brief The simulated memory, cache and DMA bus (see the file note). */
class Machine {
public:
  /// The most registered ranges.
  static constexpr std::size_t kMaxRanges = 16U;
  /// The most explicit raw DMA windows.
  static constexpr std::size_t kMaxWindows = 4U;
  /// The most cache lines held at once.
  static constexpr std::size_t kMaxLines = 64U;
  /// The largest line size the model supports.
  static constexpr std::size_t kMaxLineBytes = 128U;

  /**
   * @brief A machine with cache lines of `line_bytes` (a power of two, at most 128).
   * @param speculative_refill When true, `Speculate()` refills uncached lines of buffers a DMA
   * master owns, like a speculative load on a Cortex-M7.
   */
  explicit Machine(std::size_t line_bytes, bool speculative_refill = false) noexcept
      : line_bytes_(line_bytes), speculative_(speculative_refill) {}

  /**
   * @brief Registers a buffer's memory (its padded capacity, `Buffer::RawMemory`). `cacheable` is
   * false for a Coherent or NonCacheable region. A cacheable range must be whole lines (address and
   * size).
   * @return False when the registry is full or a cacheable range is not whole lines.
   */
  bool RegisterBuffer(std::span<std::byte> memory, const kb::dma::BufferHeader& header,
                      bool cacheable) noexcept {
    return add(memory, &header, cacheable);
  }

  /** @brief Registers raw memory (an arena of neighbours and caller buffers); same rules. */
  bool RegisterRaw(std::span<std::byte> memory, bool cacheable) noexcept {
    return add(memory, nullptr, cacheable);
  }

  /** @brief Declares [`base`, `base + size`) DMA-owned until `ClearWindows()` (raw memory has no
   * lease). */
  bool OpenWindow(const std::byte* base, std::size_t size) noexcept {
    bool ok = window_count_ < kMaxWindows;
    if (ok) {
      windows_[window_count_] = Window{.base = kb::dma::AddressOf(base), .size = size};
      ++window_count_;
    }
    return ok;
  }

  /** @brief Closes every raw DMA window. */
  void ClearWindows() noexcept {
    window_count_ = 0U;
  }

  /** @brief Turns speculative refill on or off. */
  void SetSpeculation(bool on) noexcept {
    speculative_ = on;
  }

  /** @name CPU side
   * @{ */

  /** @brief The CPU stores `data` at `dst` (through the cache for cacheable memory). */
  void CpuStore(std::byte* dst, std::span<const std::byte> data) noexcept {
    const std::uintptr_t start = kb::dma::AddressOf(dst);
    checkCpuAccess(start, data.size());
    for (std::size_t i = 0U; i < data.size(); ++i) {
      const std::uintptr_t addr = start + i;
      const Range* range = find(addr);
      if (range == nullptr) {
        continue;
      }
      if (range->cacheable) {
        Line* line = lineFor(addr, true);
        if (line != nullptr) {
          line->data[offsetIn(addr)] = data[i];
          line->dirty = true;
        }
      } else {
        memoryAt(addr) = data[i];
      }
    }
  }

  /** @brief The CPU loads `out.size()` bytes from `src` (through the cache for cacheable memory).
   */
  void CpuLoad(const std::byte* src, std::span<std::byte> out) noexcept {
    load(src, out, true);
  }

  /**
   * @brief The port's own CPU load of memory a lease holds (through `Lease::DeviceView`, as an
   * interrupt copies a finished frame out of the DMA buffer): the same, without the lease check.
   */
  void PortLoad(const std::byte* src, std::span<std::byte> out) noexcept {
    load(src, out, false);
  }
  /** @} */

  /** @name Cache maintenance
   * @{ */

  /** @brief Writes back every dirty line that overlaps `range`. */
  void CleanRange(std::span<const std::byte> range) noexcept {
    ++maint_.cleans;
    checkAligned(range.data(), range.size());
    forEachLine(kb::dma::AddressOf(range.data()), range.size(), [this](Line& line) noexcept {
      if (line.valid && line.dirty) {
        writeBack(line);
      }
    });
  }

  /** @brief Discards every line that overlaps `range` (a dirty one is a `LostWrite`). */
  void InvalidateRange(std::span<const std::byte> range) noexcept {
    ++maint_.invalidates;
    checkAligned(range.data(), range.size());
    forEachLine(kb::dma::AddressOf(range.data()), range.size(), [this](Line& line) noexcept {
      if (line.valid && line.dirty) {
        record(Violation::LostWrite, line.tag);
      }
      line.valid = false;
      line.dirty = false;
    });
  }

  /** @brief A barrier (counted only). */
  void Barrier() noexcept {
    ++maint_.barriers;
  }

  /** @brief The maintenance calls so far. */
  [[nodiscard]] MaintenanceCounts Maintenance() const noexcept {
    return maint_;
  }

  /** @brief Evicts every line (dirty lines are written back): what a busy cache does eventually. */
  void EvictAll() noexcept {
    for (Line& line : lines_) {
      if (line.valid && line.dirty) {
        writeBack(line);
      }
      line.valid = false;
      line.dirty = false;
    }
  }

  /** @brief True when `addr` is held by a valid dirty line. */
  [[nodiscard]] bool IsDirty(const std::byte* addr) const noexcept {
    const Line* line = cachedLine(kb::dma::AddressOf(addr));
    return (line != nullptr) && line->dirty;
  }

  /** @brief True when `addr` is held by a valid line. */
  [[nodiscard]] bool IsCached(const std::byte* addr) const noexcept {
    return cachedLine(kb::dma::AddressOf(addr)) != nullptr;
  }

  /** @brief A speculative load refills every uncached line of the buffers a DMA master owns now. */
  void Speculate() noexcept {
    if (speculative_) {
      for (std::size_t r = 0U; r < range_count_; ++r) {
        const Range& range = ranges_[r];
        const bool owned =
            (range.header != nullptr) && (range.header->Current() == kb::dma::BufferState::Device);
        if (owned && range.cacheable) {
          for (std::uintptr_t a = range.base; a < (range.base + range.size); a += line_bytes_) {
            static_cast<void>(lineFor(a, true));
          }
        }
      }
    }
  }
  /** @} */

  /** @name DMA side
   * @{ */

  /** @brief A DMA master writes `data` at `dst`: to memory, with the ownership checks. */
  void DmaWrite(std::byte* dst, std::span<const std::byte> data) noexcept {
    const std::uintptr_t start = kb::dma::AddressOf(dst);
    bool dirty = false;
    for (std::size_t i = 0U; i < data.size(); ++i) {
      const std::uintptr_t addr = start + i;
      if (checkDmaByte(addr)) {
        const Line* line = cachedLine(addr);
        dirty = dirty || ((line != nullptr) && line->dirty);
        memoryAt(addr) = data[i];
      }
    }
    if (dirty) {
      record(Violation::DirtyUnderDmaWrite, start);
    }
  }

  /** @brief A DMA master reads `out.size()` bytes at `src` from memory, with the ownership checks.
   */
  void DmaRead(const std::byte* src, std::span<std::byte> out) noexcept {
    const std::uintptr_t start = kb::dma::AddressOf(src);
    bool dirty = false;
    for (std::size_t i = 0U; i < out.size(); ++i) {
      const std::uintptr_t addr = start + i;
      if (checkDmaByte(addr)) {
        const Line* line = cachedLine(addr);
        dirty = dirty || ((line != nullptr) && line->dirty);
        out[i] = memoryAt(addr);
      } else {
        out[i] = std::byte{0U};
      }
    }
    if (dirty) {
      record(Violation::MissingClean, start);
    }
  }
  /** @} */

  /** @name The violation log
   * @{ */

  /** @brief How many times `kind` happened. */
  [[nodiscard]] uint32_t Count(Violation kind) const noexcept {
    return counts_[static_cast<std::size_t>(kind)];
  }

  /** @brief How many violations of any kind happened. */
  [[nodiscard]] uint32_t Total() const noexcept {
    uint32_t total = 0U;
    for (const uint32_t c : counts_) {
      total += c;
    }
    return total;
  }

  /** @brief Forgets the log (not the cache or the memory). */
  void ClearLog() noexcept {
    counts_.fill(0U);
  }

  /** @brief The cache line size of this machine. */
  [[nodiscard]] std::size_t LineBytes() const noexcept {
    return line_bytes_;
  }
  /** @} */

private:
  struct Range {
    std::uintptr_t base{0U};
    std::size_t size{0U};
    std::byte* mem{nullptr};
    const kb::dma::BufferHeader* header{nullptr};
    bool cacheable{false};
  };
  struct Window {
    std::uintptr_t base{0U};
    std::size_t size{0U};
  };
  struct Line {
    std::uintptr_t tag{0U};
    bool valid{false};
    bool dirty{false};
    std::array<std::byte, kMaxLineBytes> data{};
  };

  void load(const std::byte* src, std::span<std::byte> out, bool check_lease) noexcept {
    const std::uintptr_t start = kb::dma::AddressOf(src);
    if (check_lease) {
      checkCpuAccess(start, out.size());
    }
    bool stale = false;
    for (std::size_t i = 0U; i < out.size(); ++i) {
      const std::uintptr_t addr = start + i;
      const Range* range = find(addr);
      if (range == nullptr) {
        out[i] = std::byte{0U};
      } else if (range->cacheable) {
        const Line* hit = cachedLine(addr);
        if ((hit != nullptr) && !hit->dirty && (hit->data[offsetIn(addr)] != memoryAt(addr))) {
          stale = true;
        }
        const Line* line = (hit != nullptr) ? hit : lineFor(addr, true);
        out[i] = (line != nullptr) ? line->data[offsetIn(addr)] : std::byte{0U};
      } else {
        out[i] = memoryAt(addr);
      }
    }
    if (stale) {
      record(Violation::StaleCpuRead, start);
    }
  }

  bool add(std::span<std::byte> memory, const kb::dma::BufferHeader* header,
           bool cacheable) noexcept {
    const std::uintptr_t address = kb::dma::AddressOf(memory.data());
    const std::size_t size = memory.size();
    bool ok = range_count_ < kMaxRanges;
    if (ok && cacheable) {
      ok = ((address % line_bytes_) == 0U) && ((size % line_bytes_) == 0U);
    }
    if (ok) {
      ranges_[range_count_] = Range{.base = address,
                                    .size = size,
                                    .mem = memory.data(),
                                    .header = header,
                                    .cacheable = cacheable};
      ++range_count_;
    }
    return ok;
  }

  [[nodiscard]] const Range* find(std::uintptr_t addr) const noexcept {
    const Range* found = nullptr;
    for (std::size_t r = 0U; (r < range_count_) && (found == nullptr); ++r) {
      if ((addr >= ranges_[r].base) && ((addr - ranges_[r].base) < ranges_[r].size)) {
        found = &ranges_[r];
      }
    }
    return found;
  }

  /// The byte of real memory at `addr` (the caller has checked that `addr` is in a range).
  [[nodiscard]] std::byte& memoryAt(std::uintptr_t addr) const noexcept {
    const Range* range = find(addr);
    return (range != nullptr) ? range->mem[addr - range->base] : scratch_;
  }

  [[nodiscard]] std::size_t offsetIn(std::uintptr_t addr) const noexcept {
    return static_cast<std::size_t>(addr % line_bytes_);
  }

  [[nodiscard]] const Line* cachedLine(std::uintptr_t addr) const noexcept {
    const std::uintptr_t tag = addr - (addr % line_bytes_);
    const Line* found = nullptr;
    for (const Line& line : lines_) {
      if (line.valid && (line.tag == tag)) {
        found = &line;
      }
    }
    return found;
  }

  /// The line holding `addr`, filled from memory on a miss when `allocate`.
  Line* lineFor(std::uintptr_t addr, bool allocate) noexcept {
    const std::uintptr_t tag = addr - (addr % line_bytes_);
    Line* found = nullptr;
    for (Line& line : lines_) {
      if (line.valid && (line.tag == tag)) {
        found = &line;
      }
    }
    if ((found == nullptr) && allocate) {
      found = victim();
      found->tag = tag;
      found->valid = true;
      found->dirty = false;
      for (std::size_t i = 0U; i < line_bytes_; ++i) {
        found->data[i] = (find(tag + i) != nullptr) ? memoryAt(tag + i) : std::byte{0U};
      }
    }
    return found;
  }

  Line* victim() noexcept {
    Line* chosen = nullptr;
    for (Line& line : lines_) {
      if (!line.valid && (chosen == nullptr)) {
        chosen = &line;
      }
    }
    if (chosen == nullptr) {
      chosen = &lines_[next_victim_];
      next_victim_ = (next_victim_ + 1U) % kMaxLines;
      if (chosen->dirty) {
        writeBack(*chosen);
      }
    }
    return chosen;
  }

  void writeBack(Line& line) noexcept {
    for (std::size_t i = 0U; i < line_bytes_; ++i) {
      if (find(line.tag + i) != nullptr) {
        memoryAt(line.tag + i) = line.data[i];
      }
    }
    line.dirty = false;
  }

  template <typename Fn>
  void forEachLine(std::uintptr_t start, std::size_t size, Fn fn) noexcept {
    if (size != 0U) {
      const std::uintptr_t first = start - (start % line_bytes_);
      const std::uintptr_t last = (start + size - 1U) - ((start + size - 1U) % line_bytes_);
      for (Line& line : lines_) {
        if (line.valid && (line.tag >= first) && (line.tag <= last)) {
          fn(line);
        }
      }
    }
  }

  void checkAligned(const std::byte* base, std::size_t size) noexcept {
    const std::uintptr_t address = kb::dma::AddressOf(base);
    if (((address % line_bytes_) != 0U) || ((size % line_bytes_) != 0U)) {
      record(Violation::UnalignedMaintenance, address);
    }
  }

  void checkCpuAccess(std::uintptr_t start, std::size_t size) noexcept {
    bool leased = false;
    for (std::size_t i = 0U; i < size; ++i) {
      const Range* range = find(start + i);
      if ((range != nullptr) && (range->header != nullptr) &&
          (range->header->Current() != kb::dma::BufferState::Cpu)) {
        leased = true;
      }
    }
    if (leased) {
      record(Violation::AccessDuringLease, start);
    }
  }

  /// True when the byte may be accessed (it is registered); records an ownership violation if the
  /// DMA does not own it.
  bool checkDmaByte(std::uintptr_t addr) noexcept {
    const Range* range = find(addr);
    const bool exists = (range != nullptr);
    if (!exists) {
      record(Violation::DmaOutsideBuffer, addr);
    } else if (range->header != nullptr) {
      if (range->header->Current() == kb::dma::BufferState::Cpu) {
        record(Violation::DmaAfterReturn, addr);
      }
    } else if (!inWindow(addr)) {
      record(Violation::DmaOutsideBuffer, addr);
    } else {
      // Raw memory inside an open window: the DMA owns it.
    }
    return exists;
  }

  [[nodiscard]] bool inWindow(std::uintptr_t addr) const noexcept {
    bool inside = false;
    for (std::size_t w = 0U; w < window_count_; ++w) {
      if ((addr >= windows_[w].base) && ((addr - windows_[w].base) < windows_[w].size)) {
        inside = true;
      }
    }
    return inside;
  }

  void record(Violation kind, std::uintptr_t /*where*/) noexcept {
    ++counts_[static_cast<std::size_t>(kind)];
  }

  mutable std::byte
      scratch_{}; ///< Where an access outside every range lands (the caller checks first).
  std::size_t line_bytes_;
  bool speculative_;
  std::array<Range, kMaxRanges> ranges_{};
  std::size_t range_count_{0U};
  std::array<Window, kMaxWindows> windows_{};
  std::size_t window_count_{0U};
  std::array<Line, kMaxLines> lines_{};
  std::size_t next_victim_{0U};
  std::array<uint32_t, kViolationKinds> counts_{};
  MaintenanceCounts maint_{};
};

} // namespace kb::dma::host

#endif // KB_DMA_HOST_MACHINE_HPP
