// clang-format off: the tables below must keep one row per line.
/**
 * @file fault_journal.hpp
 * @brief kb::utils::FaultJournal<Store, Slots>: fault evidence that survives a reset. A ring of
 * fixed-size, CRC-checked records in a store the port backs (RTC or no-init RAM, NVS, FRAM,
 * EEPROM, emulated flash); `RamJournalStore<Bytes>` for host tests and RAM-backed ports.
 *
 * | Property | How |
 * | --- | --- |
 * | Fixed size, no allocation | `Slots` records of 32 bytes in the store; the journal object holds four counters |
 * | Integrity | CRC-32 (`Crc32Ieee`) over the first 28 bytes; a magic and a format version |
 * | Torn write | A reset in the middle of Write() leaves a slot whose CRC fails: Open() counts it as torn and skips it; the next append overwrites it |
 * | Wrap-around | Appends go to the slot after the newest record; the oldest record is overwritten; sequence numbers wrap and are compared modulo 2^32 |
 * | Bounded write | Append() is one store Write() of 32 bytes and no scan: its time is the store's write time |
 * | Survives a reset | Open() rebuilds the head from the store alone: the newest valid sequence |
 *
 * Record layout (little-endian):
 *
 * | Bytes | Field |
 * | --- | --- |
 * | 0-1 | magic 0x4A4B ("KJ") |
 * | 2 | format version (1) |
 * | 3 | `RecordKind` |
 * | 4-7 | sequence |
 * | 8-15 | time, microseconds (kb::Instant of the writer) |
 * | 16-17 | source (a handler or thread id the HAL assigns) |
 * | 18 | `kb::Status` |
 * | 19 | reserved, 0 |
 * | 20-23 | detail (the driver's or vendor's code) |
 * | 24-27 | value (a count, a reset reason, a liveness mask) |
 * | 28-31 | CRC-32 of bytes 0-27 |
 *
 * One writer: call Open() once at boot, then Append() from one context (the supervisor task). The
 * journal takes no lock; a store whose Write() is safe from a fault handler makes Append() usable
 * there too.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
// clang-format on
#ifndef KB_UTILS_FAULT_JOURNAL_HPP
#define KB_UTILS_FAULT_JOURNAL_HPP

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>

#include "kb/utils/crc.hpp"
#include "kb/utils/endian.hpp"
#include "kb/utils/result.hpp"
#include "kb/utils/status.hpp"
#include "kb/utils/time.hpp"

namespace kb::utils {

/** @brief What a journal record says. */
enum class RecordKind : uint8_t {
  Fault = 1,  ///< A component faulted (status, detail: its last failure; value: failures in a row).
  Reset = 2,  ///< The reset reason at boot (value: a ResetReason; detail: the port's own code).
  Health = 3, ///< A component recovered or changed health (value: the new state).
  Stall = 4,  ///< A liveness watch went stale (value: the stale mask).
  Note = 5,   ///< Anything else the HAL records.
};

/** @brief Why the MCU last reset, as the port maps its own reset cause. */
enum class ResetReason : uint8_t {
  Unknown = 0,  ///< The port cannot tell.
  PowerOn = 1,  ///< Power-on or brown-out-free cold start.
  External = 2, ///< The reset pin.
  Software = 3, ///< A requested restart.
  Watchdog = 4, ///< A watchdog (task, interrupt or hardware) fired.
  Brownout = 5, ///< Supply below threshold.
  Panic = 6,    ///< A fault handler or panic restarted the MCU.
  Wakeup = 7,   ///< Wake from a deep low-power state.
  Other = 8,    ///< A cause the port knows but kb has no name for (see detail).
};

/** @brief One journal record. The journal assigns `sequence`. */
struct FaultRecord {
  uint64_t time_us{0U};              ///< When, on the writer's clock.
  uint32_t sequence{0U};             ///< Assigned by Append().
  uint32_t detail{0U};               ///< The driver's or vendor's code.
  uint32_t value{0U};                ///< Kind-specific value.
  uint16_t source{0U};               ///< Who: an id the HAL assigns.
  kb::Status status{kb::Status::Ok}; ///< The kb::Status of a fault.
  RecordKind kind{RecordKind::Note}; ///< What the record says.
};

/** @brief The size of one record in the store. */
inline constexpr std::size_t kFaultRecordBytes = 32U;

/** @brief The bytes of one record. */
using FaultRecordBytes = std::array<uint8_t, kFaultRecordBytes>;

namespace detail {
inline constexpr uint16_t kJournalMagic = 0x4A4BU;   ///< "KJ", little-endian.
inline constexpr uint8_t kJournalVersion = 0x01U;    ///< Format version.
inline constexpr std::size_t kAtVersion = 0x02U;     ///< Offset of the version.
inline constexpr std::size_t kAtKind = 0x03U;        ///< Offset of the kind.
inline constexpr std::size_t kAtSequence = 0x04U;    ///< Offset of the sequence.
inline constexpr std::size_t kAtTime = 0x08U;        ///< Offset of the time.
inline constexpr std::size_t kAtSource = 0x10U;      ///< Offset of the source.
inline constexpr std::size_t kAtStatus = 0x12U;      ///< Offset of the status.
inline constexpr std::size_t kAtReserved = 0x13U;    ///< Offset of the reserved byte.
inline constexpr std::size_t kAtDetail = 0x14U;      ///< Offset of the detail.
inline constexpr std::size_t kAtValue = 0x18U;       ///< Offset of the value.
inline constexpr std::size_t kAtCrc = 0x1CU;         ///< Offset of the CRC (and bytes it covers).
inline constexpr uint8_t kLastKind = 0x05U;          ///< The highest RecordKind value.
inline constexpr uint32_t kHalfRange = 0x80000000UL; ///< Sequence comparison window.

/** @brief True when sequence `a` is newer than `b` (modulo 2^32, within half the range). */
[[nodiscard]] constexpr bool SequenceNewer(uint32_t a, uint32_t b) noexcept {
  const uint32_t d = a - b;
  return (d != 0U) && (d < kHalfRange);
}
} // namespace detail

/** @brief The 32 bytes of `record`, with its CRC. */
[[nodiscard]] constexpr FaultRecordBytes EncodeRecord(const FaultRecord& record) noexcept {
  FaultRecordBytes b{};
  const std::span<uint8_t, kFaultRecordBytes> out{b};
  StoreLe16(detail::kJournalMagic, out.first<2U>());
  b[detail::kAtVersion] = detail::kJournalVersion;
  b[detail::kAtKind] = static_cast<uint8_t>(record.kind);
  StoreLe32(record.sequence, out.subspan<detail::kAtSequence, 4U>());
  StoreLe64(record.time_us, out.subspan<detail::kAtTime, 8U>());
  StoreLe16(record.source, out.subspan<detail::kAtSource, 2U>());
  b[detail::kAtStatus] = static_cast<uint8_t>(record.status);
  StoreLe32(record.detail, out.subspan<detail::kAtDetail, 4U>());
  StoreLe32(record.value, out.subspan<detail::kAtValue, 4U>());
  const std::span<const uint8_t> covered = std::span<const uint8_t>{b}.first(detail::kAtCrc);
  StoreLe32(Crc32Ieee::Compute(covered), out.subspan<detail::kAtCrc, 4U>());
  return b;
}

/**
 * @brief The record in `bytes`.
 * @return Empty when the magic is absent (never written, erased to 0xFF or 0x00); CrcMismatch for a
 * torn or corrupt slot; Unsupported for another format version, a reserved byte set, or a kind or
 * status this build does not know (a newer writer).
 */
[[nodiscard]] constexpr kb::Result<FaultRecord> ParseRecord(
    std::span<const uint8_t, kFaultRecordBytes> bytes) noexcept {
  kb::Result<FaultRecord> result = kb::Result<FaultRecord>::Failure(kb::Status::Empty);
  const uint8_t kind = bytes[detail::kAtKind];
  const uint8_t status = bytes[detail::kAtStatus];
  if (LoadLe16(bytes.first<2U>()) != detail::kJournalMagic) {
    // Empty.
  } else if (LoadLe32(bytes.subspan<detail::kAtCrc, 4U>()) !=
             Crc32Ieee::Compute(bytes.first(detail::kAtCrc))) {
    result = kb::Result<FaultRecord>::Failure(kb::Status::CrcMismatch);
  } else if ((bytes[detail::kAtVersion] != detail::kJournalVersion) ||
             (bytes[detail::kAtReserved] != 0U) || (kind == 0U) || (kind > detail::kLastKind) ||
             (status >= kStatusCount)) {
    result = kb::Result<FaultRecord>::Failure(kb::Status::Unsupported);
  } else {
    FaultRecord r{};
    r.kind = static_cast<RecordKind>(kind);
    r.sequence = LoadLe32(bytes.subspan<detail::kAtSequence, 4U>());
    r.time_us = LoadLe64(bytes.subspan<detail::kAtTime, 8U>());
    r.source = LoadLe16(bytes.subspan<detail::kAtSource, 2U>());
    r.status = static_cast<kb::Status>(status);
    r.detail = LoadLe32(bytes.subspan<detail::kAtDetail, 4U>());
    r.value = LoadLe32(bytes.subspan<detail::kAtValue, 4U>());
    result = kb::Result<FaultRecord>::Success(r);
  }
  return result;
}

/**
 * @brief Where a journal lives. `Read` and `Write` move bytes at a byte offset and return within a
 * bounded time; `Capacity()` is the store's size in bytes. A flash-backed store hides erase and
 * write units behind Write() (or places each 32-byte slot in its own write unit).
 */
template <typename S>
concept JournalStore = requires(S& store, const S& cstore, std::size_t offset,
                                std::span<const uint8_t> in, std::span<uint8_t> out) {
  { cstore.Capacity() } noexcept -> std::same_as<std::size_t>;
  { store.Read(offset, out) } noexcept -> std::same_as<kb::Status>;
  { store.Write(offset, in) } noexcept -> std::same_as<kb::Status>;
};

/**
 * @brief A journal store in RAM: host tests, and ports that place it in RAM kept across a reset
 * (an RTC or no-init section; the port's linker places the object, kb-core does not).
 * @tparam Bytes The size of the store.
 */
template <std::size_t Bytes>
  requires(Bytes >= kFaultRecordBytes)
class RamJournalStore {
public:
  constexpr RamJournalStore() noexcept = default;

  /** @brief JournalStore::Capacity. */
  [[nodiscard]] constexpr std::size_t Capacity() const noexcept {
    return Bytes;
  }
  /** @brief JournalStore::Read; OutOfRange past the end. */
  [[nodiscard]] constexpr kb::Status Read(std::size_t offset, std::span<uint8_t> out) noexcept {
    const bool fits = fitsAt(offset, out.size());
    if (fits) {
      for (std::size_t i = 0U; i < out.size(); ++i) {
        out[i] = bytes_[offset + i];
      }
    }
    return fits ? kb::Status::Ok : kb::Status::OutOfRange;
  }
  /** @brief JournalStore::Write; OutOfRange past the end. */
  [[nodiscard]] constexpr kb::Status Write(std::size_t offset,
                                           std::span<const uint8_t> in) noexcept {
    const bool fits = fitsAt(offset, in.size());
    if (fits) {
      for (std::size_t i = 0U; i < in.size(); ++i) {
        bytes_[offset + i] = in[i];
      }
    }
    return fits ? kb::Status::Ok : kb::Status::OutOfRange;
  }
  /** @brief The raw bytes (tests corrupt and inspect them). */
  [[nodiscard]] constexpr std::span<uint8_t, Bytes> Raw() noexcept {
    return std::span<uint8_t, Bytes>{bytes_};
  }

private:
  [[nodiscard]] static constexpr bool fitsAt(std::size_t offset, std::size_t size) noexcept {
    return (offset <= Bytes) && (size <= (Bytes - offset));
  }

  std::array<uint8_t, Bytes> bytes_{};
};

/** @brief What Open() found in the store. */
struct JournalScan {
  uint32_t valid{0U};       ///< Slots holding a good record.
  uint32_t empty{0U};       ///< Slots never written (or erased).
  uint32_t torn{0U};        ///< Slots whose CRC fails: a torn write or corruption.
  uint32_t unsupported{0U}; ///< Slots written by another format version or a newer writer.
  uint32_t newest{0U};      ///< The newest sequence; valid when `valid` is not 0.
};

/** @brief The journal's counters since construction. */
struct JournalStats {
  uint32_t appends{0U};        ///< Records written.
  uint32_t write_failures{0U}; ///< Appends the store refused (nothing advanced).
};

/**
 * @brief A ring of `Slots` records in `Store` (see the file note).
 * @tparam Store A JournalStore of at least Slots x 32 bytes.
 * @tparam Slots Records kept, 2 to 4096.
 */
template <JournalStore Store, std::size_t Slots>
  requires((Slots >= 2U) && (Slots <= 4096U))
class FaultJournal {
public:
  /** @brief Over `store`; no I/O (Open() reads it). */
  constexpr explicit FaultJournal(Store& store) noexcept : store_(store) {}

  /**
   * @brief Reads every slot once and continues after the newest record (bounded: Slots reads).
   * @return The scan; OutOfRange when the store is smaller than Slots records; a store read
   * failure as the store reported it (the journal stays closed).
   */
  [[nodiscard]] kb::Result<JournalScan> Open() noexcept {
    kb::Result<JournalScan> result = kb::Result<JournalScan>::Failure(kb::Status::OutOfRange);
    if (store_.Capacity() >= (Slots * kFaultRecordBytes)) {
      JournalScan scan{};
      std::size_t newest_slot = Slots - 1U; // so that an empty journal starts at slot 0
      kb::Status failed = kb::Status::Ok;
      for (std::size_t i = 0U; (i < Slots) && (failed == kb::Status::Ok); ++i) {
        FaultRecordBytes raw{};
        failed = store_.Read(i * kFaultRecordBytes, raw);
        const kb::Result<FaultRecord> r = ParseRecord(raw);
        if (failed != kb::Status::Ok) {
          // Reported below.
        } else if (r.IsOk()) {
          if ((scan.valid == 0U) || detail::SequenceNewer(r.Value().sequence, scan.newest)) {
            scan.newest = r.Value().sequence;
            newest_slot = i;
          }
          ++scan.valid;
        } else {
          count(r.Error(), scan);
        }
      }
      if (failed == kb::Status::Ok) {
        head_ = (newest_slot + 1U) % Slots;
        next_ = (scan.valid == 0U) ? 1U : (scan.newest + 1U);
        count_ = scan.valid;
        open_ = true;
        result = kb::Result<JournalScan>::Success(scan);
      } else {
        result = kb::Result<JournalScan>::Failure(failed);
      }
    }
    return result;
  }

  /**
   * @brief Writes `record` in the next slot with the next sequence (one store write, no scan).
   * @return The sequence; NotReady before Open(); the store's failure (nothing advanced).
   */
  [[nodiscard]] kb::Result<uint32_t> Append(FaultRecord record) noexcept {
    kb::Result<uint32_t> result = kb::Result<uint32_t>::Failure(kb::Status::NotReady);
    if (open_) {
      record.sequence = next_;
      const FaultRecordBytes raw = EncodeRecord(record);
      const kb::Status s = store_.Write(head_ * kFaultRecordBytes, raw);
      if (s == kb::Status::Ok) {
        head_ = (head_ + 1U) % Slots;
        ++next_;
        count_ = (count_ < Slots) ? (count_ + 1U) : Slots;
        ++stats_.appends;
        result = kb::Result<uint32_t>::Success(record.sequence);
      } else {
        ++stats_.write_failures;
        result = kb::Result<uint32_t>::Failure(s);
      }
    }
    return result;
  }

  /** @brief Records the reset reason at boot: `reason` and the port's own `port_code`. */
  [[nodiscard]] kb::Result<uint32_t> RecordReset(ResetReason reason, uint32_t port_code,
                                                 kb::Instant now) noexcept {
    FaultRecord r{};
    r.kind = RecordKind::Reset;
    r.time_us = now.Us();
    r.detail = port_code;
    r.value = static_cast<uint32_t>(reason);
    return Append(r);
  }

  /**
   * @brief The record `age` appends before the newest (0 is the newest), read from the store.
   * @return NotReady before Open(); OutOfRange when `age` is not below Count(); a slot that no
   * longer parses as ParseRecord() reports it.
   */
  [[nodiscard]] kb::Result<FaultRecord> Read(std::size_t age) noexcept {
    kb::Result<FaultRecord> result = kb::Result<FaultRecord>::Failure(kb::Status::NotReady);
    if (!open_) {
      // NotReady.
    } else if (age >= count_) {
      result = kb::Result<FaultRecord>::Failure(kb::Status::OutOfRange);
    } else {
      const std::size_t slot = ((head_ + Slots) - 1U - age) % Slots;
      FaultRecordBytes raw{};
      const kb::Status s = store_.Read(slot * kFaultRecordBytes, raw);
      result = (s == kb::Status::Ok) ? ParseRecord(raw) : kb::Result<FaultRecord>::Failure(s);
    }
    return result;
  }

  /** @brief The newest record of `kind` (bounded: at most Count() reads); Empty when none. */
  [[nodiscard]] kb::Result<FaultRecord> FindNewest(RecordKind kind) noexcept {
    kb::Result<FaultRecord> result =
        kb::Result<FaultRecord>::Failure(open_ ? kb::Status::Empty : kb::Status::NotReady);
    bool found = false;
    for (std::size_t age = 0U; (age < count_) && (!found); ++age) {
      const kb::Result<FaultRecord> r = Read(age);
      if (r.IsOk() && (r.Value().kind == kind)) {
        result = r;
        found = true;
      }
    }
    return result;
  }

  /** @brief Slots that hold a record (valid ones found by Open() plus appends, at most Slots). */
  [[nodiscard]] std::size_t Count() const noexcept {
    return count_;
  }

  /** @brief True after a successful Open(). */
  [[nodiscard]] bool IsOpen() const noexcept {
    return open_;
  }

  /** @brief A copy of the counters. */
  [[nodiscard]] JournalStats Stats() const noexcept {
    return stats_;
  }

private:
  static void count(kb::Status status, JournalScan& scan) noexcept {
    if (status == kb::Status::Empty) {
      ++scan.empty;
    } else if (status == kb::Status::CrcMismatch) {
      ++scan.torn;
    } else {
      ++scan.unsupported;
    }
  }

  Store& store_;
  JournalStats stats_{};
  std::size_t head_{0U};
  std::size_t count_{0U};
  uint32_t next_{1U};
  bool open_{false};
};

} // namespace kb::utils

#endif // KB_UTILS_FAULT_JOURNAL_HPP
