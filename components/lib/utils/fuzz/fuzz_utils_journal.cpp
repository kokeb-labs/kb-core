// Fuzz target: FaultJournal record parsing and the scan of an arbitrary store. Any 32 bytes parse
// to a record that encodes back to the same bytes, or to one of the documented refusals; any store
// content opens with every slot counted once and takes an append that reads back as the newest;
// when the store is a coherent journal (every valid sequence within Slots of the newest, as a
// journal writes them), a reopen after the append (a reset) finds that record again.
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>

#include "kb/utils/fault_journal.hpp"

namespace {

void Require(bool condition) {
  if (!condition) {
    std::abort();
  }
}

constexpr std::size_t kSlots = 6U;
using Store = kb::utils::RamJournalStore<kSlots * kb::utils::kFaultRecordBytes>;

void CheckParse(std::span<const uint8_t, kb::utils::kFaultRecordBytes> bytes) {
  const kb::Result<kb::utils::FaultRecord> r = kb::utils::ParseRecord(bytes);
  if (r.IsOk()) {
    const kb::utils::FaultRecordBytes again = kb::utils::EncodeRecord(r.Value());
    for (std::size_t i = 0U; i < again.size(); ++i) {
      Require(again[i] == bytes[i]);
    }
  } else {
    const kb::Status e = r.Error();
    Require((e == kb::Status::Empty) || (e == kb::Status::CrcMismatch) ||
            (e == kb::Status::Unsupported));
  }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, std::size_t size) {
  const std::span<const uint8_t> input{data, size};
  if (input.size() >= kb::utils::kFaultRecordBytes) {
    CheckParse(input.first<kb::utils::kFaultRecordBytes>());
  }
  Store store{};
  const std::span<uint8_t> raw = store.Raw();
  for (std::size_t i = 0U; (i < input.size()) && (i < raw.size()); ++i) {
    raw[i] = input[i];
  }
  std::array<uint32_t, kSlots> seqs{};
  std::size_t valid = 0U;
  for (std::size_t i = 0U; i < kSlots; ++i) {
    const auto slot =
        store.Raw().subspan(i * kb::utils::kFaultRecordBytes).first<kb::utils::kFaultRecordBytes>();
    CheckParse(slot);
    const kb::Result<kb::utils::FaultRecord> r = kb::utils::ParseRecord(slot);
    if (r.IsOk()) {
      seqs[valid] = r.Value().sequence;
      ++valid;
    }
  }
  kb::utils::FaultJournal<Store, kSlots> journal{store};
  const kb::Result<kb::utils::JournalScan> scan = journal.Open();
  Require(scan.IsOk());
  const kb::utils::JournalScan& s = scan.Value();
  Require((s.valid + s.empty + s.torn + s.unsupported) == kSlots);
  Require(s.valid == valid);
  bool coherent = true;
  for (std::size_t i = 0U; i < valid; ++i) {
    coherent = coherent && ((s.newest - seqs[i]) < kSlots);
  }
  Require(journal.Count() == s.valid);
  kb::utils::FaultRecord note{};
  note.kind = kb::utils::RecordKind::Note;
  note.value = static_cast<uint32_t>(size);
  const kb::Result<uint32_t> seq = journal.Append(note);
  Require(seq.IsOk());
  Require((s.valid == 0U) || (seq.Value() == (s.newest + 1U)));
  const kb::Result<kb::utils::FaultRecord> newest = journal.Read(0U);
  Require(newest.IsOk() && (newest.Value().sequence == seq.Value()));

  kb::utils::FaultJournal<Store, kSlots> reopened{store};
  const kb::Result<kb::utils::JournalScan> again = reopened.Open();
  Require(again.IsOk());
  const kb::Result<kb::utils::FaultRecord> found = reopened.Read(0U);
  Require(found.IsOk());
  Require((!coherent) ||
          ((found.Value().sequence == seq.Value()) && (found.Value().value == note.value)));
  return 0;
}
