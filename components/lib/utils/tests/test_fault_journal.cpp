// kb::utils::FaultJournal, its record format and RamJournalStore: the byte layout against a golden
// record, every parse refusal, store bounds, the scan of a store after a reset (empty, valid, torn,
// unsupported, a failing store), wrap-around, sequence wrap, reads by age, the newest record of a
// kind, the reset-reason record, a refused write and a torn write in the middle of an append.
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "kb/utils/fault_journal.hpp"
#include "kb_test.hpp"

using kb::Status;
using kb::utils::FaultJournal;
using kb::utils::FaultRecord;
using kb::utils::FaultRecordBytes;
using kb::utils::RamJournalStore;
using kb::utils::RecordKind;
using kb::utils::ResetReason;
using kb_test::Rt;

namespace {

constexpr std::size_t kSlots = 4U;
using Store = RamJournalStore<kSlots * kb::utils::kFaultRecordBytes>;
using Journal = FaultJournal<Store, kSlots>;

FaultRecord Sample() {
  FaultRecord r{};
  r.kind = RecordKind::Fault;
  r.sequence = Rt<uint32_t>(0x04030201U);
  r.time_us = Rt<uint64_t>(0x1112131415161718ULL);
  r.source = Rt<uint16_t>(0x2122U);
  r.status = Status::Timeout;
  r.detail = Rt<uint32_t>(0x31323334U);
  r.value = Rt<uint32_t>(0x41424344U);
  return r;
}

// The bytes of Sample(): every field at its documented offset, little-endian, then the CRC-32.
constexpr FaultRecordBytes kGolden{0x4BU, 0x4AU, 0x01U, 0x01U, 0x01U, 0x02U, 0x03U, 0x04U,
                                   0x18U, 0x17U, 0x16U, 0x15U, 0x14U, 0x13U, 0x12U, 0x11U,
                                   0x22U, 0x21U, 0x03U, 0x00U, 0x34U, 0x33U, 0x32U, 0x31U,
                                   0x44U, 0x43U, 0x42U, 0x41U, 0x00U, 0x00U, 0x00U, 0x00U};

FaultRecordBytes Golden() {
  FaultRecordBytes g = kGolden;
  const uint32_t crc = kb::utils::Crc32Ieee::Compute(std::span<const uint8_t>{g}.first(28U));
  g[28] = static_cast<uint8_t>(crc & 0xFFU);
  g[29] = static_cast<uint8_t>((crc >> 8U) & 0xFFU);
  g[30] = static_cast<uint8_t>((crc >> 16U) & 0xFFU);
  g[31] = static_cast<uint8_t>((crc >> 24U) & 0xFFU);
  return g;
}

bool Same(const FaultRecord& a, const FaultRecord& b) {
  return (a.time_us == b.time_us) && (a.sequence == b.sequence) && (a.detail == b.detail) &&
         (a.value == b.value) && (a.source == b.source) && (a.status == b.status) &&
         (a.kind == b.kind);
}

FaultRecord Note(uint32_t value) {
  FaultRecord r{};
  r.kind = RecordKind::Note;
  r.value = value;
  return r;
}

/// Slot `slot` of the store, as bytes.
std::span<uint8_t, kb::utils::kFaultRecordBytes> SlotOf(Store& store, std::size_t slot) {
  return store.Raw()
      .subspan(slot * kb::utils::kFaultRecordBytes)
      .first<kb::utils::kFaultRecordBytes>();
}

/// A store that fails on command: reads, or writes (after writing the first `torn_after` bytes,
/// as a reset in the middle of a write leaves them).
class FaultyStore {
public:
  Store inner;
  bool fail_read{false};
  bool fail_write{false};
  std::size_t torn_after{0U};

  [[nodiscard]] std::size_t Capacity() const noexcept {
    return inner.Capacity();
  }
  [[nodiscard]] Status Read(std::size_t offset, std::span<uint8_t> out) noexcept {
    return fail_read ? Status::BusError : inner.Read(offset, out);
  }
  [[nodiscard]] Status Write(std::size_t offset, std::span<const uint8_t> in) noexcept {
    Status s = Status::Ok;
    if (fail_write) {
      static_cast<void>(inner.Write(offset, in.first(torn_after)));
      // cppcheck-suppress redundantInitialization
      s = Status::Fault;
    } else {
      s = inner.Write(offset, in);
    }
    return s;
  }
};
// The numbers are the stored format: a record written before a reset is read after it, by this
// build or the next one.
static_assert((static_cast<uint8_t>(RecordKind::Fault) == 1U) &&
              (static_cast<uint8_t>(RecordKind::Reset) == 2U) &&
              (static_cast<uint8_t>(RecordKind::Health) == 3U) &&
              (static_cast<uint8_t>(RecordKind::Stall) == 4U) &&
              (static_cast<uint8_t>(RecordKind::Note) == 5U));
static_assert((static_cast<uint8_t>(ResetReason::Unknown) == 0U) &&
              (static_cast<uint8_t>(ResetReason::PowerOn) == 1U) &&
              (static_cast<uint8_t>(ResetReason::External) == 2U) &&
              (static_cast<uint8_t>(ResetReason::Software) == 3U) &&
              (static_cast<uint8_t>(ResetReason::Watchdog) == 4U) &&
              (static_cast<uint8_t>(ResetReason::Brownout) == 5U) &&
              (static_cast<uint8_t>(ResetReason::Panic) == 6U) &&
              (static_cast<uint8_t>(ResetReason::Wakeup) == 7U) &&
              (static_cast<uint8_t>(ResetReason::Other) == 8U));
static_assert(kb::utils::kFaultRecordBytes == 32U);

// The template bounds: 2 to 4096 slots; a store holds at least one record.
template <std::size_t N>
concept SlotsAccepted = requires { typename FaultJournal<Store, N>; };
static_assert(SlotsAccepted<2U> && SlotsAccepted<4096U> && (!SlotsAccepted<1U>) &&
              (!SlotsAccepted<4097U>));
template <std::size_t B>
concept StoreAccepted = requires { typename RamJournalStore<B>; };
static_assert(StoreAccepted<32U> && (!StoreAccepted<31U>));
static_assert(kb::utils::JournalStore<FaultyStore>);
static_assert(kb::utils::JournalStore<Store>);

} // namespace

KB_TEST("journal record: a default record is a Note of zeros with status Ok") {
  const FaultRecord r{};
  KB_CHECK_EQ(r.time_us, 0U);
  KB_CHECK_EQ(r.sequence, 0U);
  KB_CHECK_EQ(r.detail, 0U);
  KB_CHECK_EQ(r.value, 0U);
  KB_CHECK_EQ(r.source, 0U);
  KB_CHECK_EQ(r.status, Status::Ok);
  KB_CHECK(r.kind == RecordKind::Note);
}

KB_TEST("journal record: the bytes are the documented layout, and parse back to the record") {
  const FaultRecordBytes bytes = kb::utils::EncodeRecord(Sample());
  const FaultRecordBytes golden = Golden();
  KB_CHECK(bytes == golden);
  const kb::Result<FaultRecord> back = kb::utils::ParseRecord(golden);
  KB_REQUIRE(back.IsOk());
  KB_CHECK(Same(back.Value(), Sample()));
}

KB_TEST("journal record: every kind and status round-trips; the edges of each range") {
  for (uint8_t k = 1U; k <= 5U; ++k) {
    FaultRecord r = Sample();
    r.kind = static_cast<RecordKind>(Rt(k));
    r.status = static_cast<Status>(Rt<uint8_t>(15U));
    const kb::Result<FaultRecord> back = kb::utils::ParseRecord(kb::utils::EncodeRecord(r));
    KB_REQUIRE(back.IsOk());
    KB_CHECK(Same(back.Value(), r));
  }
}

KB_TEST("journal record: an absent magic is Empty, a bad CRC is a torn slot") {
  FaultRecordBytes erased{};
  erased.fill(0xFFU);
  KB_CHECK_EQ(kb::utils::ParseRecord(erased).Error(), Status::Empty);
  const FaultRecordBytes zero{};
  KB_CHECK_EQ(kb::utils::ParseRecord(zero).Error(), Status::Empty);
  FaultRecordBytes half = Golden();
  half[0] = 0x4BU;
  half[1] = 0x00U; // the second magic byte alone is wrong
  KB_CHECK_EQ(kb::utils::ParseRecord(half).Error(), Status::Empty);
  for (std::size_t i = 2U; i < kb::utils::kFaultRecordBytes; ++i) {
    FaultRecordBytes torn = Golden();
    torn[i] = static_cast<uint8_t>(torn[i] ^ 0x01U);
    KB_CHECK_EQ(kb::utils::ParseRecord(torn).Error(), Status::CrcMismatch);
  }
}

KB_TEST("journal record: another version, a reserved byte, an unknown kind or status is "
        "Unsupported") {
  const auto with = [](std::size_t at, uint8_t value) {
    FaultRecordBytes b = kb::utils::EncodeRecord(Sample());
    b[at] = value;
    const uint32_t crc = kb::utils::Crc32Ieee::Compute(std::span<const uint8_t>{b}.first(28U));
    kb::utils::StoreLe32(crc, std::span<uint8_t, 32>{b}.subspan<28U, 4U>());
    return kb::utils::ParseRecord(b).Error();
  };
  KB_CHECK_EQ(with(2U, 0x02U), Status::Unsupported);  // version 2
  KB_CHECK_EQ(with(2U, 0x00U), Status::Unsupported);  // version 0
  KB_CHECK_EQ(with(19U, 0x01U), Status::Unsupported); // reserved byte
  KB_CHECK_EQ(with(3U, 0x00U), Status::Unsupported);  // kind 0
  KB_CHECK_EQ(with(3U, 0x06U), Status::Unsupported);  // kind past Note
  KB_CHECK_EQ(with(18U, 16U), Status::Unsupported);   // status past Fault
  KB_CHECK_EQ(with(3U, 0x05U), Status::Ok);           // Note is known
  KB_CHECK_EQ(with(18U, 15U), Status::Ok);            // Fault is known
}

KB_TEST("journal store: reads and writes inside the store; OutOfRange past its end") {
  RamJournalStore<32U> store{};
  KB_CHECK_EQ(store.Capacity(), 32U);
  const std::array<uint8_t, 4> in{1U, 2U, 3U, 4U};
  KB_CHECK_EQ(store.Write(Rt<std::size_t>(28U), in), Status::Ok);
  std::array<uint8_t, 4> out{};
  KB_CHECK_EQ(store.Read(Rt<std::size_t>(28U), out), Status::Ok);
  KB_CHECK(out == in);
  KB_CHECK_EQ(store.Write(Rt<std::size_t>(29U), in), Status::OutOfRange);
  KB_CHECK_EQ(store.Read(Rt<std::size_t>(29U), out), Status::OutOfRange);
  const std::span<const uint8_t> none{};
  KB_CHECK_EQ(store.Write(Rt<std::size_t>(32U), none), Status::Ok); // nothing, at the end
  KB_CHECK_EQ(store.Write(Rt<std::size_t>(33U), none), Status::OutOfRange);
  KB_CHECK_EQ(store.Read(Rt<std::size_t>(40U), std::span<uint8_t>{}), Status::OutOfRange);
  const std::array<uint8_t, 33> big{};
  KB_CHECK_EQ(store.Write(Rt<std::size_t>(0U), big), Status::OutOfRange);
  KB_CHECK_EQ(store.Raw()[31], 4U); // the refused writes changed nothing
}

KB_TEST("journal: nothing before Open(); a store smaller than the slots does not open") {
  Store store{};
  Journal j{store};
  KB_CHECK(!j.IsOpen());
  KB_CHECK_EQ(j.Count(), 0U);
  KB_CHECK_EQ(j.Append(Note(1U)).Error(), Status::NotReady);
  KB_CHECK_EQ(j.Read(0U).Error(), Status::NotReady);
  KB_CHECK_EQ(j.FindNewest(RecordKind::Note).Error(), Status::NotReady);
  RamJournalStore<(kSlots * 32U) - 1U> small{};
  FaultJournal<RamJournalStore<(kSlots * 32U) - 1U>, kSlots> tiny{small};
  KB_CHECK_EQ(tiny.Open().Error(), Status::OutOfRange);
  KB_CHECK(!tiny.IsOpen());
  RamJournalStore<(kSlots * 32U) + 1U> roomy{};
  FaultJournal<RamJournalStore<(kSlots * 32U) + 1U>, kSlots> fits{roomy};
  KB_CHECK(fits.Open().IsOk()); // a larger store is fine
}

KB_TEST("journal: an empty store opens empty; the first record goes to slot 0 with sequence 1") {
  Store store{};
  Journal j{store};
  const kb::Result<kb::utils::JournalScan> scan = j.Open();
  KB_REQUIRE(scan.IsOk());
  KB_CHECK_EQ(scan.Value().empty, kSlots);
  KB_CHECK_EQ(scan.Value().valid, 0U);
  KB_CHECK_EQ(scan.Value().newest, 0U);
  KB_CHECK_EQ(scan.Value().torn, 0U);
  KB_CHECK_EQ(scan.Value().unsupported, 0U);
  KB_CHECK_EQ(j.Count(), 0U);
  KB_CHECK_EQ(j.Stats().appends, 0U);
  KB_CHECK_EQ(j.Stats().write_failures, 0U);
  KB_CHECK_EQ(j.Read(0U).Error(), Status::OutOfRange);
  KB_CHECK_EQ(j.FindNewest(RecordKind::Note).Error(), Status::Empty);
  const kb::Result<uint32_t> seq = j.Append(Note(7U));
  KB_REQUIRE(seq.IsOk());
  KB_CHECK_EQ(seq.Value(), 1U);
  KB_CHECK_EQ(kb::utils::ParseRecord(SlotOf(store, 0U)).Value().value, 7U);
  KB_CHECK_EQ(kb::utils::ParseRecord(SlotOf(store, 1U)).Error(), Status::Empty);
  KB_CHECK_EQ(j.Count(), 1U);
  KB_CHECK_EQ(j.Stats().appends, 1U);
}

KB_TEST("journal: wrap-around keeps the newest Slots records, oldest overwritten") {
  Store store{};
  Journal j{store};
  KB_REQUIRE(j.Open().IsOk());
  for (uint32_t v = 1U; v <= 6U; ++v) {
    KB_REQUIRE(j.Append(Note(v * 10U)).IsOk());
    KB_CHECK_EQ(j.Count(), (v < kSlots) ? v : kSlots); // never more than the slots
  }
  KB_CHECK_EQ(j.Count(), kSlots);
  for (std::size_t age = 0U; age < kSlots; ++age) {
    const kb::Result<FaultRecord> r = j.Read(Rt(age));
    KB_REQUIRE(r.IsOk());
    KB_CHECK_EQ(r.Value().value, (6U - age) * 10U);
    KB_CHECK_EQ(r.Value().sequence, 6U - age);
  }
  KB_CHECK_EQ(j.Read(kSlots).Error(), Status::OutOfRange);
  KB_CHECK_EQ(kb::utils::ParseRecord(SlotOf(store, 0U)).Value().value, 50U); // the 5th append
  KB_CHECK_EQ(kb::utils::ParseRecord(SlotOf(store, 1U)).Value().value, 60U);
}

KB_TEST("journal: after a reset, Open() continues after the newest record") {
  Store store{}; // the RAM kept across the reset
  {
    Journal before{store};
    KB_REQUIRE(before.Open().IsOk());
    for (uint32_t v = 1U; v <= 5U; ++v) {
      KB_REQUIRE(before.Append(Note(v)).IsOk());
    }
  }
  Journal after{store};
  const kb::Result<kb::utils::JournalScan> scan = after.Open();
  KB_REQUIRE(scan.IsOk());
  KB_CHECK_EQ(scan.Value().valid, kSlots);
  KB_CHECK_EQ(scan.Value().newest, 5U);
  KB_CHECK_EQ(after.Count(), kSlots);
  KB_CHECK_EQ(after.Read(0U).Value().value, 5U);
  const kb::Result<uint32_t> seq = after.Append(Note(6U));
  KB_REQUIRE(seq.IsOk());
  KB_CHECK_EQ(seq.Value(), 6U);
  KB_CHECK_EQ(kb::utils::ParseRecord(SlotOf(store, 1U)).Value().value, 6U); // after slot 0
  KB_CHECK_EQ(after.Read(1U).Value().value, 5U);
  KB_CHECK_EQ(after.Read(3U).Value().value, 3U);
}

KB_TEST("journal: a partly filled store reopens with its count and its next slot") {
  Store store{};
  {
    Journal before{store};
    KB_REQUIRE(before.Open().IsOk());
    KB_REQUIRE(before.Append(Note(1U)).IsOk());
    KB_REQUIRE(before.Append(Note(2U)).IsOk());
  }
  Journal after{store};
  const kb::Result<kb::utils::JournalScan> scan = after.Open();
  KB_REQUIRE(scan.IsOk());
  KB_CHECK_EQ(scan.Value().valid, 2U);
  KB_CHECK_EQ(scan.Value().empty, 2U);
  KB_CHECK_EQ(after.Count(), 2U);
  KB_CHECK_EQ(after.Read(2U).Error(), Status::OutOfRange);
  KB_REQUIRE(after.Append(Note(3U)).IsOk());
  KB_CHECK_EQ(kb::utils::ParseRecord(SlotOf(store, 2U)).Value().value, 3U);
  KB_CHECK_EQ(after.Count(), 3U);
}

KB_TEST("journal: the newest is found across a sequence wrap") {
  Store store{};
  std::array<uint32_t, kSlots> seqs{0xFFFFFFFEU, 0xFFFFFFFFU, 0x00000000U, 0x00000001U};
  for (std::size_t i = 0U; i < kSlots; ++i) {
    FaultRecord r = Note(static_cast<uint32_t>(i));
    r.sequence = seqs[i];
    const FaultRecordBytes b = kb::utils::EncodeRecord(r);
    KB_REQUIRE(store.Write(i * 32U, b) == Status::Ok);
  }
  Journal j{store};
  const kb::Result<kb::utils::JournalScan> scan = j.Open();
  KB_REQUIRE(scan.IsOk());
  KB_CHECK_EQ(scan.Value().newest, 1U);
  KB_CHECK_EQ(j.Read(0U).Value().value, 3U);
  KB_CHECK_EQ(j.Append(Note(9U)).Value(), 2U);
  KB_CHECK_EQ(kb::utils::ParseRecord(SlotOf(store, 0U)).Value().value, 9U); // replaced the oldest

  // The same sequences in another order: the newest is still 1, wherever it is.
  Store other{};
  std::array<uint32_t, kSlots> mixed{0x00000001U, 0xFFFFFFFEU, 0x00000000U, 0xFFFFFFFFU};
  for (std::size_t i = 0U; i < kSlots; ++i) {
    FaultRecord r = Note(static_cast<uint32_t>(i));
    r.sequence = mixed[i];
    KB_REQUIRE(other.Write(i * 32U, kb::utils::EncodeRecord(r)) == Status::Ok);
  }
  Journal k{other};
  KB_REQUIRE(k.Open().IsOk());
  KB_CHECK_EQ(k.Read(0U).Value().value, 0U);
  // A record half the range ahead is not newer.
  KB_CHECK(!kb::utils::detail::SequenceNewer(Rt(0x80000000U), Rt(0U)));
  KB_CHECK(kb::utils::detail::SequenceNewer(Rt(0x7FFFFFFFU), Rt(0U)));
  KB_CHECK(!kb::utils::detail::SequenceNewer(Rt(5U), Rt(5U)));
}

KB_TEST("journal: a torn write is detected at Open(), skipped, and overwritten next") {
  FaultyStore store{};
  {
    FaultJournal<FaultyStore, kSlots> before{store};
    KB_REQUIRE(before.Open().IsOk());
    KB_REQUIRE(before.Append(Note(1U)).IsOk());
    KB_REQUIRE(before.Append(Note(2U)).IsOk());
    store.fail_write = true;
    store.torn_after = 13U; // the reset hits in the middle of the third record
    KB_CHECK_EQ(before.Append(Note(3U)).Error(), Status::Fault);
    KB_CHECK_EQ(before.Stats().write_failures, 1U);
    KB_CHECK_EQ(before.Count(), 2U); // nothing advanced
  }
  // cppcheck-suppress redundantAssignment
  store.fail_write = false;
  FaultJournal<FaultyStore, kSlots> after{store};
  const kb::Result<kb::utils::JournalScan> scan = after.Open();
  KB_REQUIRE(scan.IsOk());
  KB_CHECK_EQ(scan.Value().valid, 2U);
  KB_CHECK_EQ(scan.Value().torn, 1U);
  KB_CHECK_EQ(scan.Value().empty, 1U);
  KB_CHECK_EQ(scan.Value().unsupported, 0U);
  KB_CHECK_EQ(after.Read(0U).Value().value, 2U); // the evidence before the reset is intact
  KB_REQUIRE(after.Append(Note(4U)).IsOk());
  KB_CHECK_EQ(kb::utils::ParseRecord(SlotOf(store.inner, 2U)).Value().value, 4U);
  KB_CHECK_EQ(after.Read(0U).Value().sequence, 3U);
}

KB_TEST("journal: unsupported slots are counted and skipped; a failing store read fails Open()") {
  Store store{};
  FaultRecordBytes b = kb::utils::EncodeRecord(Note(1U));
  b[2] = 0x07U; // a later format version, with its own CRC
  kb::utils::StoreLe32(kb::utils::Crc32Ieee::Compute(std::span<const uint8_t>{b}.first(28U)),
                       std::span<uint8_t, 32>{b}.subspan<28U, 4U>());
  KB_REQUIRE(store.Write(32U, b) == Status::Ok);
  Journal j{store};
  const kb::Result<kb::utils::JournalScan> scan = j.Open();
  KB_REQUIRE(scan.IsOk());
  KB_CHECK_EQ(scan.Value().unsupported, 1U);
  KB_CHECK_EQ(scan.Value().empty, 3U);

  FaultyStore bad{};
  bad.fail_read = true;
  FaultJournal<FaultyStore, kSlots> k{bad};
  KB_CHECK_EQ(k.Open().Error(), Status::BusError);
  KB_CHECK(!k.IsOpen());
}

KB_TEST("journal: a slot corrupted after Open() reads as torn; a failing read is reported") {
  FaultyStore store{};
  FaultJournal<FaultyStore, kSlots> j{store};
  KB_REQUIRE(j.Open().IsOk());
  KB_REQUIRE(j.Append(Note(1U)).IsOk());
  KB_REQUIRE(j.Append(Note(2U)).IsOk());
  SlotOf(store.inner, 0U)[9] ^= 0x10U;
  KB_CHECK_EQ(j.Read(1U).Error(), Status::CrcMismatch);
  KB_CHECK(j.Read(0U).IsOk());
  // cppcheck-suppress unreadVariable
  store.fail_read = true;
  KB_CHECK_EQ(j.Read(0U).Error(), Status::BusError);
}

KB_TEST("journal: the reset reason, and the newest record of a kind") {
  Store store{};
  Journal j{store};
  KB_REQUIRE(j.Open().IsOk());
  KB_REQUIRE(j.RecordReset(ResetReason::Watchdog, 0x0FU, kb::Instant{Rt<uint64_t>(1234U)}).IsOk());
  FaultRecord f{};
  f.kind = RecordKind::Fault;
  f.source = 3U;
  KB_REQUIRE(j.Append(f).IsOk());
  KB_REQUIRE(j.Append(Note(5U)).IsOk());
  const kb::Result<FaultRecord> reset = j.FindNewest(RecordKind::Reset);
  KB_REQUIRE(reset.IsOk());
  KB_CHECK(reset.Value().kind == RecordKind::Reset);
  KB_CHECK_EQ(reset.Value().value, static_cast<uint32_t>(ResetReason::Watchdog));
  KB_CHECK_EQ(reset.Value().detail, 0x0FU);
  KB_CHECK_EQ(reset.Value().time_us, 1234U);
  KB_CHECK_EQ(reset.Value().status, Status::Ok);
  KB_CHECK_EQ(reset.Value().source, 0U);
  KB_CHECK_EQ(j.FindNewest(RecordKind::Fault).Value().source, 3U);
  KB_CHECK_EQ(j.FindNewest(RecordKind::Note).Value().value, 5U); // the newest is age 0
  KB_CHECK_EQ(j.FindNewest(RecordKind::Stall).Error(), Status::Empty);
  KB_REQUIRE(j.Append(Note(6U)).IsOk());
  KB_CHECK_EQ(j.FindNewest(RecordKind::Note).Value().value, 6U);
  SlotOf(store, 3U)[5] ^= 0x01U; // the newest Note is corrupted: the one before it is found
  KB_CHECK_EQ(j.FindNewest(RecordKind::Note).Value().value, 5U);
}
