// Tests of the span helpers and FixedString (span.hpp, fixed_string.hpp).
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "kb/utils/fixed_string.hpp"
#include "kb/utils/span.hpp"
#include "kb_test.hpp"

namespace u = kb::utils;

KB_TEST("span: checked sub-spans never pass the end, also for huge offsets") {
  std::array<uint8_t, 4> b{1U, 2U, 3U, 4U};
  std::span<uint8_t> out{};
  KB_CHECK(u::CheckedSubspan(std::span<uint8_t>{b}, kb_test::Rt(1U), kb_test::Rt(2U), out) &&
           (out.size() == kb_test::Rt(2U)) && (out[0] == 2U));
  KB_CHECK(u::CheckedSubspan(std::span<uint8_t>{b}, kb_test::Rt(4U), kb_test::Rt(0U), out) &&
           out.empty());
  KB_CHECK(!u::CheckedSubspan(std::span<uint8_t>{b}, kb_test::Rt(3U), kb_test::Rt(2U), out));
  KB_CHECK(!u::CheckedSubspan(std::span<uint8_t>{b}, kb_test::Rt(5U), kb_test::Rt(0U), out));
  KB_CHECK(!u::CheckedSubspan(std::span<uint8_t>{b}, kb_test::Rt(1U), SIZE_MAX, out));
}

KB_TEST("span: copy, fill and equality") {
  std::array<uint8_t, 3> dst{};
  std::array<uint8_t, 2> src{7U, 8U};
  std::array<uint8_t, 4> big{};
  KB_CHECK(u::CopyInto<uint8_t>(dst, std::span<const uint8_t>{src}) && (dst[1] == 8U) &&
           (dst[2] == 0U));
  KB_CHECK(
      !u::CopyInto<uint8_t>(dst, std::span<const uint8_t>{big.data(), kb_test::Rt(big.size())}));
  u::Fill<uint8_t>(dst, 0x55U);
  KB_CHECK_EQ(dst[2], 0x55U);
  const std::array<uint8_t, 3> same{0x55U, 0x55U, 0x55U};
  const std::array<uint8_t, 3> other{0x55U, 0x56U, 0x55U};
  KB_CHECK(u::Equal<uint8_t>(dst, same));
  KB_CHECK(!u::Equal<uint8_t>(dst, other));
  KB_CHECK(!u::Equal<uint8_t>(dst, src));
  KB_CHECK(u::ConstantTimeEqual(dst, same));
  KB_CHECK(!u::ConstantTimeEqual(dst, other));
  KB_CHECK(
      !u::ConstantTimeEqual(dst, std::span<const uint8_t>{src.data(), kb_test::Rt(src.size())}));
  const uint32_t word = 0x01020304UL;
  KB_CHECK_EQ(u::ObjectBytes(word).size(), kb_test::Rt(4U));
}

KB_TEST("fixed string: appends, numbers in several bases, and truncation") {
  u::FixedString<32> s{};
  KB_CHECK(s.Append("tick=") && s.AppendUnsigned(kb_test::Rt(1234U)) && s.Append(" hex=") &&
           s.AppendHex(kb_test::Rt(0x2AU), kb_test::Rt(4U)));
  KB_CHECK(s.View() == "tick=1234 hex=002A");
  KB_CHECK_EQ(s.Length(), 18U);
  KB_CHECK(std::string_view{s.CStr()} == s.View());
  s.Clear();
  KB_CHECK(s.AppendSigned(kb_test::Rt(-42)) && s.AppendChar(' ') &&
           s.AppendSigned(kb_test::Rt(INT64_MIN)));
  KB_CHECK(s.View() == "-42 -9223372036854775808");
  s.Clear();
  KB_CHECK(s.AppendUnsigned(kb_test::Rt(5U), kb_test::Rt(2U)) && s.AppendBool(true) &&
           s.AppendBool(false) && s.AppendUnsigned(kb_test::Rt(0U)));
  KB_CHECK(s.View() == "101truefalse0");
  KB_CHECK(!s.AppendUnsigned(kb_test::Rt(1U), kb_test::Rt(1U)) &&
           !s.AppendUnsigned(kb_test::Rt(1U), kb_test::Rt(17U)));
  KB_CHECK(!s.Truncated());
  KB_CHECK_EQ(u::FixedString<32>::Capacity(), 32U);
  s.Clear();
  KB_CHECK(s.AppendSigned(kb_test::Rt(int64_t{12})) && (s.View() == "12"));
  u::FixedString<1> one{};
  KB_CHECK(!one.AppendSigned(kb_test::Rt(int64_t{12})));
}

KB_TEST("fixed string: an append that does not fit is cut, terminated and flagged") {
  u::FixedString<4> s{};
  KB_CHECK(!s.Append("abcdef"));
  KB_CHECK(s.View() == "abcd");
  KB_CHECK(s.Truncated());
  KB_CHECK_EQ(s.CStr()[4], '\0');
  KB_CHECK(!s.AppendSigned(kb_test::Rt(-1)));
  s.Clear();
  KB_CHECK(!s.AppendUnsigned(kb_test::Rt(UINT64_MAX), kb_test::Rt(2U), kb_test::Rt(100U)));
  KB_CHECK_EQ(s.Length(), 4U);
  KB_CHECK(!s.Truncated() || (s.View() == "1111"));
}
