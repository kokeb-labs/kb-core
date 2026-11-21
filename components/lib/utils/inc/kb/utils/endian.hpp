/**
 * @file endian.hpp
 * @brief Big- and little-endian loads and stores, and bounded byte readers and writers.
 *
 * - `LoadBe16(bytes)`, `StoreLe32(value, bytes)`, ...: fixed-extent spans, so the length is checked
 * at compile time. They do not depend on the host's byte order.
 * - `ByteReader` and `ByteWriter`: a cursor over a span for frames of mixed fields. Every read and
 * write is bounded; the first one that does not fit fails, changes nothing, and makes the cursor
 * fail from then on
 *   (`Ok()` is false), so a frame can be parsed in a straight line and checked once at the end.
 *
 * @code
 * kb::utils::ByteReader reader{frame};
 * uint8_t id = 0U;
 * uint16_t raw = 0U;
 * const bool parsed = reader.ReadU8(id) && reader.ReadBe16(raw) && reader.AtEnd();
 * @endcode
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_UTILS_ENDIAN_HPP
#define KB_UTILS_ENDIAN_HPP

#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

namespace kb::utils {

/** @brief Two bytes, most significant first. */
[[nodiscard]] constexpr uint16_t LoadBe16(std::span<const uint8_t, 2> b) noexcept {
  return static_cast<uint16_t>((static_cast<uint32_t>(b[0]) << 8U) | static_cast<uint32_t>(b[1]));
}
/** @brief Two bytes, least significant first. */
[[nodiscard]] constexpr uint16_t LoadLe16(std::span<const uint8_t, 2> b) noexcept {
  return static_cast<uint16_t>((static_cast<uint32_t>(b[1]) << 8U) | static_cast<uint32_t>(b[0]));
}
/** @brief Four bytes, most significant first. */
[[nodiscard]] constexpr uint32_t LoadBe32(std::span<const uint8_t, 4> b) noexcept {
  return (static_cast<uint32_t>(b[0]) << 24U) | (static_cast<uint32_t>(b[1]) << 16U) |
         (static_cast<uint32_t>(b[2]) << 8U) | static_cast<uint32_t>(b[3]);
}
/** @brief Four bytes, least significant first. */
[[nodiscard]] constexpr uint32_t LoadLe32(std::span<const uint8_t, 4> b) noexcept {
  return (static_cast<uint32_t>(b[3]) << 24U) | (static_cast<uint32_t>(b[2]) << 16U) |
         (static_cast<uint32_t>(b[1]) << 8U) | static_cast<uint32_t>(b[0]);
}
/** @brief Eight bytes, most significant first. */
[[nodiscard]] constexpr uint64_t LoadBe64(std::span<const uint8_t, 8> b) noexcept {
  uint64_t value = 0U;
  for (const uint8_t byte : b) {
    value = (value << 8U) | static_cast<uint64_t>(byte);
  }
  return value;
}
/** @brief Eight bytes, least significant first. */
[[nodiscard]] constexpr uint64_t LoadLe64(std::span<const uint8_t, 8> b) noexcept {
  uint64_t value = 0U;
  for (std::size_t i = 8U; i > 0U; --i) {
    value = (value << 8U) | static_cast<uint64_t>(b[i - 1U]);
  }
  return value;
}

/** @brief `value` as two bytes, most significant first. */
constexpr void StoreBe16(uint16_t value, std::span<uint8_t, 2> b) noexcept {
  const auto v = static_cast<uint32_t>(value);
  b[0] = static_cast<uint8_t>(v >> 8U);
  b[1] = static_cast<uint8_t>(v & 0xFFU);
}
/** @brief `value` as two bytes, least significant first. */
constexpr void StoreLe16(uint16_t value, std::span<uint8_t, 2> b) noexcept {
  const auto v = static_cast<uint32_t>(value);
  b[0] = static_cast<uint8_t>(v & 0xFFU);
  b[1] = static_cast<uint8_t>(v >> 8U);
}
/** @brief `value` as four bytes, most significant first. */
constexpr void StoreBe32(uint32_t value, std::span<uint8_t, 4> b) noexcept {
  b[0] = static_cast<uint8_t>(value >> 24U);
  b[1] = static_cast<uint8_t>((value >> 16U) & 0xFFU);
  b[2] = static_cast<uint8_t>((value >> 8U) & 0xFFU);
  b[3] = static_cast<uint8_t>(value & 0xFFU);
}
/** @brief `value` as four bytes, least significant first. */
constexpr void StoreLe32(uint32_t value, std::span<uint8_t, 4> b) noexcept {
  b[0] = static_cast<uint8_t>(value & 0xFFU);
  b[1] = static_cast<uint8_t>((value >> 8U) & 0xFFU);
  b[2] = static_cast<uint8_t>((value >> 16U) & 0xFFU);
  b[3] = static_cast<uint8_t>(value >> 24U);
}
/** @brief `value` as eight bytes, most significant first. */
constexpr void StoreBe64(uint64_t value, std::span<uint8_t, 8> b) noexcept {
  uint64_t v = value;
  for (std::size_t i = 8U; i > 0U; --i) {
    b[i - 1U] = static_cast<uint8_t>(v & 0xFFU);
    v >>= 8U;
  }
}
/** @brief `value` as eight bytes, least significant first. */
constexpr void StoreLe64(uint64_t value, std::span<uint8_t, 8> b) noexcept {
  uint64_t v = value;
  for (uint8_t& byte : b) {
    byte = static_cast<uint8_t>(v & 0xFFU);
    v >>= 8U;
  }
}

/** @brief A bounded cursor that reads fields from a byte span; fails sticky on the first overrun.
 */
class ByteReader {
public:
  /** @brief A reader at the start of `data`. The span must outlive the reader. */
  constexpr explicit ByteReader(std::span<const uint8_t> data) noexcept : data_(data) {}

  /** @brief One byte. */
  [[nodiscard]] constexpr bool ReadU8(uint8_t& out) noexcept {
    std::span<const uint8_t> field{};
    const bool ok = take(1U, field);
    if (ok) {
      out = field[0];
    }
    return ok;
  }
  /** @brief A 16-bit big-endian field. */
  [[nodiscard]] constexpr bool ReadBe16(uint16_t& out) noexcept {
    std::span<const uint8_t> field{};
    const bool ok = take(2U, field);
    if (ok) {
      out = LoadBe16(field.first<2>());
    }
    return ok;
  }
  /** @brief A 16-bit little-endian field. */
  [[nodiscard]] constexpr bool ReadLe16(uint16_t& out) noexcept {
    std::span<const uint8_t> field{};
    const bool ok = take(2U, field);
    if (ok) {
      out = LoadLe16(field.first<2>());
    }
    return ok;
  }
  /** @brief A 32-bit big-endian field. */
  [[nodiscard]] constexpr bool ReadBe32(uint32_t& out) noexcept {
    std::span<const uint8_t> field{};
    const bool ok = take(4U, field);
    if (ok) {
      out = LoadBe32(field.first<4>());
    }
    return ok;
  }
  /** @brief A 32-bit little-endian field. */
  [[nodiscard]] constexpr bool ReadLe32(uint32_t& out) noexcept {
    std::span<const uint8_t> field{};
    const bool ok = take(4U, field);
    if (ok) {
      out = LoadLe32(field.first<4>());
    }
    return ok;
  }
  /** @brief A 64-bit big-endian field. */
  [[nodiscard]] constexpr bool ReadBe64(uint64_t& out) noexcept {
    std::span<const uint8_t> field{};
    const bool ok = take(8U, field);
    if (ok) {
      out = LoadBe64(field.first<8>());
    }
    return ok;
  }
  /** @brief A 64-bit little-endian field. */
  [[nodiscard]] constexpr bool ReadLe64(uint64_t& out) noexcept {
    std::span<const uint8_t> field{};
    const bool ok = take(8U, field);
    if (ok) {
      out = LoadLe64(field.first<8>());
    }
    return ok;
  }
  /** @brief `out.size()` bytes copied into `out`. */
  [[nodiscard]] constexpr bool ReadBytes(std::span<uint8_t> out) noexcept {
    std::span<const uint8_t> field{};
    const bool ok = take(out.size(), field);
    if (ok) {
      for (std::size_t i = 0U; i < field.size(); ++i) {
        out[i] = field[i];
      }
    }
    return ok;
  }
  /** @brief Skips `count` bytes. */
  [[nodiscard]] constexpr bool Skip(std::size_t count) noexcept {
    std::span<const uint8_t> field{};
    return take(count, field);
  }

  /** @brief Bytes not yet read; 0 after a failure. */
  [[nodiscard]] constexpr std::size_t Remaining() const noexcept {
    return failed_ ? 0U : (data_.size() - position_);
  }
  /** @brief Bytes read so far. */
  [[nodiscard]] constexpr std::size_t Position() const noexcept {
    return position_;
  }
  /** @brief True while every read so far fitted. */
  [[nodiscard]] constexpr bool Ok() const noexcept {
    return !failed_;
  }
  /** @brief True when no read failed and every byte was read. */
  [[nodiscard]] constexpr bool AtEnd() const noexcept {
    return (!failed_) && (position_ == data_.size());
  }

private:
  [[nodiscard]] constexpr bool take(std::size_t count, std::span<const uint8_t>& field) noexcept {
    const bool fits = (!failed_) && (count <= (data_.size() - position_));
    if (fits) {
      field = data_.subspan(position_, count);
      position_ += count;
    } else {
      failed_ = true;
    }
    return fits;
  }

  std::span<const uint8_t> data_;
  std::size_t position_{0U};
  bool failed_{false};
};

/** @brief A bounded cursor that writes fields into a byte span; fails sticky on the first overrun.
 */
class ByteWriter {
public:
  /** @brief A writer at the start of `out`. The span must outlive the writer. */
  constexpr explicit ByteWriter(std::span<uint8_t> out) noexcept : data_(out) {}

  /** @brief One byte. */
  [[nodiscard]] constexpr bool WriteU8(uint8_t value) noexcept {
    std::span<uint8_t> field{};
    const bool ok = take(1U, field);
    if (ok) {
      field[0] = value;
    }
    return ok;
  }
  /** @brief A 16-bit big-endian field. */
  [[nodiscard]] constexpr bool WriteBe16(uint16_t value) noexcept {
    std::span<uint8_t> field{};
    const bool ok = take(2U, field);
    if (ok) {
      StoreBe16(value, field.first<2>());
    }
    return ok;
  }
  /** @brief A 16-bit little-endian field. */
  [[nodiscard]] constexpr bool WriteLe16(uint16_t value) noexcept {
    std::span<uint8_t> field{};
    const bool ok = take(2U, field);
    if (ok) {
      StoreLe16(value, field.first<2>());
    }
    return ok;
  }
  /** @brief A 32-bit big-endian field. */
  [[nodiscard]] constexpr bool WriteBe32(uint32_t value) noexcept {
    std::span<uint8_t> field{};
    const bool ok = take(4U, field);
    if (ok) {
      StoreBe32(value, field.first<4>());
    }
    return ok;
  }
  /** @brief A 32-bit little-endian field. */
  [[nodiscard]] constexpr bool WriteLe32(uint32_t value) noexcept {
    std::span<uint8_t> field{};
    const bool ok = take(4U, field);
    if (ok) {
      StoreLe32(value, field.first<4>());
    }
    return ok;
  }
  /** @brief A 64-bit big-endian field. */
  [[nodiscard]] constexpr bool WriteBe64(uint64_t value) noexcept {
    std::span<uint8_t> field{};
    const bool ok = take(8U, field);
    if (ok) {
      StoreBe64(value, field.first<8>());
    }
    return ok;
  }
  /** @brief A 64-bit little-endian field. */
  [[nodiscard]] constexpr bool WriteLe64(uint64_t value) noexcept {
    std::span<uint8_t> field{};
    const bool ok = take(8U, field);
    if (ok) {
      StoreLe64(value, field.first<8>());
    }
    return ok;
  }
  /** @brief `bytes` copied in. */
  [[nodiscard]] constexpr bool WriteBytes(std::span<const uint8_t> bytes) noexcept {
    std::span<uint8_t> field{};
    const bool ok = take(bytes.size(), field);
    if (ok) {
      for (std::size_t i = 0U; i < bytes.size(); ++i) {
        field[i] = bytes[i];
      }
    }
    return ok;
  }

  /** @brief The bytes written so far. */
  [[nodiscard]] constexpr std::span<const uint8_t> Written() const noexcept {
    return std::span<const uint8_t>{data_.first(position_)};
  }
  /** @brief Bytes still free; 0 after a failure. */
  [[nodiscard]] constexpr std::size_t Remaining() const noexcept {
    return failed_ ? 0U : (data_.size() - position_);
  }
  /** @brief True while every write so far fitted. */
  [[nodiscard]] constexpr bool Ok() const noexcept {
    return !failed_;
  }

private:
  [[nodiscard]] constexpr bool take(std::size_t count, std::span<uint8_t>& field) noexcept {
    const bool fits = (!failed_) && (count <= (data_.size() - position_));
    if (fits) {
      field = data_.subspan(position_, count);
      position_ += count;
    } else {
      failed_ = true;
    }
    return fits;
  }

  std::span<uint8_t> data_;
  std::size_t position_{0U};
  bool failed_{false};
};

/** @brief The bits of a signed 16-bit field read as unsigned (two's complement, std::bit_cast). */
[[nodiscard]] constexpr int16_t AsSigned16(uint16_t raw) noexcept {
  return std::bit_cast<int16_t>(raw);
}
/** @brief The bits of a signed 32-bit field read as unsigned (two's complement, std::bit_cast). */
[[nodiscard]] constexpr int32_t AsSigned32(uint32_t raw) noexcept {
  return std::bit_cast<int32_t>(raw);
}

} // namespace kb::utils

#endif // KB_UTILS_ENDIAN_HPP
