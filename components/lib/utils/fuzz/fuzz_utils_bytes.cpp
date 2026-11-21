// Fuzz target: ByteReader and ByteWriter driven by an opcode stream never read or write out of
// bounds, fail sticky, and a value written then read back is the value. SPDX-License-Identifier:
// MIT Copyright (c) 2026 The kb-core authors
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>

#include "kb/utils/endian.hpp"

namespace {

void Require(bool condition) {
  if (!condition) {
    std::abort();
  }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, std::size_t size) {
  const std::span<const uint8_t> in{data, size};
  kb::utils::ByteReader reader{in};
  std::array<uint8_t, 24> out{};
  const std::size_t out_size =
      (size == 0U) ? 0U : (static_cast<std::size_t>(data[0]) % (out.size() + 1U));
  kb::utils::ByteWriter writer{std::span<uint8_t>{out}.first(out_size)};
  uint8_t op = 0U;
  while (reader.ReadU8(op)) {
    bool was_ok = writer.Ok();
    bool wrote = false;
    uint16_t v16 = 0U;
    uint32_t v32 = 0U;
    uint64_t v64 = 0U;
    switch (op % 8U) {
    case 0U:
      wrote = reader.ReadBe16(v16) && writer.WriteBe16(v16);
      break;
    case 1U:
      wrote = reader.ReadLe16(v16) && writer.WriteLe16(v16);
      break;
    case 2U:
      wrote = reader.ReadBe32(v32) && writer.WriteBe32(v32);
      break;
    case 3U:
      wrote = reader.ReadLe32(v32) && writer.WriteLe32(v32);
      break;
    case 4U:
      wrote = reader.ReadBe64(v64) && writer.WriteBe64(v64);
      break;
    case 5U:
      wrote = reader.ReadLe64(v64) && writer.WriteLe64(v64);
      break;
    case 6U:
      wrote = reader.Skip(op / 8U);
      break;
    default: {
      std::array<uint8_t, 5> chunk{};
      wrote = reader.ReadBytes(chunk) && writer.WriteBytes(chunk);
      break;
    }
    }
    // Once failed, a writer never succeeds again.
    Require(was_ok || !writer.Ok());
    static_cast<void>(wrote);
    Require(writer.Written().size() <= out_size);
    Require(reader.Position() <= size);
  }
  Require(!reader.Ok() || reader.AtEnd());
  // Round trip of the first bytes through both byte orders.
  if (size >= 8U) {
    const uint64_t be = kb::utils::LoadBe64(in.first<8>());
    std::array<uint8_t, 8> back{};
    kb::utils::StoreBe64(be, back);
    for (std::size_t i = 0U; i < 8U; ++i) {
      Require(back[i] == data[i]);
    }
    kb::utils::StoreLe64(kb::utils::LoadLe64(in.first<8>()), back);
    for (std::size_t i = 0U; i < 8U; ++i) {
      Require(back[i] == data[i]);
    }
  }
  return 0;
}
