// Tests of the checked and saturating arithmetic (numeric.hpp), at every limit of several widths.
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include <cstdint>
#include <limits>

#include "kb/utils/numeric.hpp"
#include "kb_test.hpp"

namespace {

template <typename T>
using L = std::numeric_limits<T>;

template <typename T>
void CheckUnsignedOps() {
  T out = 5U;
  KB_CHECK(kb::utils::CheckedAdd<T>(kb_test::Rt(T{1}), kb_test::Rt(T{2}), out) && (out == T{3}));
  KB_CHECK(!kb::utils::CheckedAdd<T>(kb_test::Rt(L<T>::max()), kb_test::Rt(T{1}), out) &&
           (out == T{3}));
  KB_CHECK(kb::utils::CheckedSub<T>(kb_test::Rt(T{5}), kb_test::Rt(T{2}), out) && (out == T{3}));
  KB_CHECK(!kb::utils::CheckedSub<T>(kb_test::Rt(T{2}), kb_test::Rt(T{5}), out));
  KB_CHECK(kb::utils::CheckedMul<T>(kb_test::Rt(T{6}), kb_test::Rt(T{7}), out) && (out == T{42}));
  KB_CHECK(kb::utils::CheckedMul<T>(kb_test::Rt(T{0}), kb_test::Rt(L<T>::max()), out) &&
           (out == T{0}));
  KB_CHECK(kb::utils::CheckedMul<T>(kb_test::Rt(L<T>::max()), kb_test::Rt(T{0}), out) &&
           (out == T{0}));
  KB_CHECK(!kb::utils::CheckedMul<T>(kb_test::Rt(L<T>::max()), kb_test::Rt(T{2}), out));
  KB_CHECK(kb::utils::SaturatingAdd<T>(kb_test::Rt(L<T>::max()), kb_test::Rt(T{1})) == L<T>::max());
  KB_CHECK(kb::utils::SaturatingAdd<T>(kb_test::Rt(T{1}), kb_test::Rt(T{1})) == T{2});
  KB_CHECK(kb::utils::SaturatingSub<T>(kb_test::Rt(T{0}), kb_test::Rt(T{1})) == T{0});
  KB_CHECK(kb::utils::SaturatingSub<T>(kb_test::Rt(T{3}), kb_test::Rt(T{1})) == T{2});
  KB_CHECK(kb::utils::SaturatingMul<T>(kb_test::Rt(L<T>::max()), kb_test::Rt(T{3})) == L<T>::max());
  KB_CHECK(kb::utils::SaturatingMul<T>(kb_test::Rt(T{3}), kb_test::Rt(T{3})) == T{9});
}

template <typename T>
void CheckSignedOps() {
  T out = 0;
  KB_CHECK(kb::utils::CheckedAdd<T>(kb_test::Rt(T{-1}), kb_test::Rt(T{2}), out) && (out == T{1}));
  KB_CHECK(!kb::utils::CheckedAdd<T>(kb_test::Rt(L<T>::max()), kb_test::Rt(T{1}), out));
  KB_CHECK(!kb::utils::CheckedAdd<T>(kb_test::Rt(L<T>::min()), kb_test::Rt(T{-1}), out));
  KB_CHECK(kb::utils::CheckedAdd<T>(kb_test::Rt(L<T>::min()), kb_test::Rt(T{0}), out) &&
           (out == L<T>::min()));
  KB_CHECK(kb::utils::CheckedSub<T>(kb_test::Rt(T{-1}), kb_test::Rt(T{2}), out) && (out == T{-3}));
  KB_CHECK(!kb::utils::CheckedSub<T>(kb_test::Rt(L<T>::min()), kb_test::Rt(T{1}), out));
  KB_CHECK(!kb::utils::CheckedSub<T>(kb_test::Rt(L<T>::max()), kb_test::Rt(T{-1}), out));
  KB_CHECK(kb::utils::CheckedSub<T>(kb_test::Rt(T{0}), kb_test::Rt(T{0}), out) && (out == T{0}));
  KB_CHECK(kb::utils::CheckedMul<T>(kb_test::Rt(T{-6}), kb_test::Rt(T{7}), out) && (out == T{-42}));
  KB_CHECK(kb::utils::CheckedMul<T>(kb_test::Rt(T{6}), kb_test::Rt(T{-7}), out) && (out == T{-42}));
  KB_CHECK(kb::utils::CheckedMul<T>(kb_test::Rt(T{-6}), kb_test::Rt(T{-7}), out) && (out == T{42}));
  KB_CHECK(kb::utils::CheckedMul<T>(kb_test::Rt(T{0}), kb_test::Rt(T{-7}), out) && (out == T{0}));
  KB_CHECK(!kb::utils::CheckedMul<T>(kb_test::Rt(L<T>::max()), kb_test::Rt(T{2}), out));
  KB_CHECK(!kb::utils::CheckedMul<T>(kb_test::Rt(L<T>::max()), kb_test::Rt(T{-2}), out));
  KB_CHECK(!kb::utils::CheckedMul<T>(kb_test::Rt(L<T>::min()), kb_test::Rt(T{2}), out));
  KB_CHECK(!kb::utils::CheckedMul<T>(kb_test::Rt(L<T>::min()), kb_test::Rt(T{-1}), out));
  KB_CHECK(kb::utils::SaturatingAdd<T>(kb_test::Rt(L<T>::max()), kb_test::Rt(T{1})) == L<T>::max());
  KB_CHECK(kb::utils::SaturatingAdd<T>(kb_test::Rt(L<T>::min()), kb_test::Rt(T{-1})) ==
           L<T>::min());
  KB_CHECK(kb::utils::SaturatingSub<T>(kb_test::Rt(L<T>::min()), kb_test::Rt(T{1})) == L<T>::min());
  KB_CHECK(kb::utils::SaturatingSub<T>(kb_test::Rt(L<T>::max()), kb_test::Rt(T{-1})) ==
           L<T>::max());
  KB_CHECK(kb::utils::SaturatingMul<T>(kb_test::Rt(L<T>::min()), kb_test::Rt(T{2})) == L<T>::min());
  KB_CHECK(kb::utils::SaturatingMul<T>(kb_test::Rt(L<T>::min()), kb_test::Rt(T{-2})) ==
           L<T>::max());
  KB_CHECK(kb::utils::SaturatingMul<T>(kb_test::Rt(T{-2}), kb_test::Rt(T{3})) == T{-6});
  KB_CHECK(kb::utils::SaturatingAdd<T>(kb_test::Rt(T{-1}), kb_test::Rt(T{2})) == T{1});
  KB_CHECK(kb::utils::SaturatingSub<T>(kb_test::Rt(T{-1}), kb_test::Rt(T{2})) == T{-3});
}

} // namespace

KB_TEST("numeric: unsigned checked and saturating arithmetic at the limits") {
  CheckUnsignedOps<uint8_t>();
  CheckUnsignedOps<uint16_t>();
  CheckUnsignedOps<uint32_t>();
  CheckUnsignedOps<uint64_t>();
}

KB_TEST("numeric: signed checked and saturating arithmetic at the limits") {
  CheckSignedOps<int8_t>();
  CheckSignedOps<int16_t>();
  CheckSignedOps<int32_t>();
  CheckSignedOps<int64_t>();
}

KB_TEST("numeric: range-checked conversion and saturation") {
  uint8_t u8 = 1U;
  KB_CHECK(kb::utils::InRangeCast<uint8_t>(kb_test::Rt(255), u8) && (u8 == 255U));
  KB_CHECK(!kb::utils::InRangeCast<uint8_t>(kb_test::Rt(256), u8) && (u8 == 255U));
  KB_CHECK(!kb::utils::InRangeCast<uint8_t>(kb_test::Rt(-1), u8));
  int8_t i8 = 0;
  KB_CHECK(kb::utils::InRangeCast<int8_t>(kb_test::Rt(-128), i8) && (i8 == -128));
  KB_CHECK(!kb::utils::InRangeCast<int8_t>(kb_test::Rt(uint64_t{200}), i8));
  KB_CHECK(kb::utils::InRange<uint16_t>(kb_test::Rt(65535)));
  KB_CHECK(!kb::utils::InRange<uint16_t>(kb_test::Rt(-1)));
  KB_CHECK_EQ(kb::utils::Saturate<uint8_t>(kb_test::Rt(-5)), 0U);
  KB_CHECK_EQ(kb::utils::Saturate<uint8_t>(kb_test::Rt(300)), 255U);
  KB_CHECK_EQ(kb::utils::Saturate<uint8_t>(kb_test::Rt(17)), 17U);
  KB_CHECK_EQ(kb::utils::Saturate<int16_t>(kb_test::Rt(uint64_t{1} << 40U)), 32767);
  KB_CHECK_EQ(kb::utils::Saturate<int16_t>(kb_test::Rt(INT64_MIN)), -32768);
}

KB_TEST("numeric: clamp and ceiling division") {
  KB_CHECK_EQ(kb::utils::Clamp(kb_test::Rt(5), 0, 10), 5);
  KB_CHECK_EQ(kb::utils::Clamp(kb_test::Rt(-5), 0, 10), 0);
  KB_CHECK_EQ(kb::utils::Clamp(kb_test::Rt(50), 0, 10), 10);
  KB_CHECK_EQ(kb::utils::Clamp(kb_test::Rt(5), 10, 0), 10);
  KB_CHECK_EQ(kb::utils::DivCeil(kb_test::Rt(10U), 3U), 4U);
  KB_CHECK_EQ(kb::utils::DivCeil(kb_test::Rt(9U), 3U), 3U);
  KB_CHECK_EQ(kb::utils::DivCeil(kb_test::Rt(9U), 0U), 0U);
  KB_CHECK_EQ(kb::utils::DivCeil(kb_test::Rt(UINT32_MAX), 2U), 0x80000000U);
  KB_CHECK_EQ(kb::utils::DivCeil(kb_test::Rt(uint8_t{255}), uint8_t{2}), 128U);
}

static_assert(kb::utils::SaturatingAdd<uint8_t>(250U, 10U) == 255U);
static_assert(kb::utils::Saturate<int8_t>((1000)) == 127);
