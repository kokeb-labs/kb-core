// Tests of kb::Status and kb::Result<T> (status.hpp, result.hpp).
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include <cstdint>
#include <string_view>

#include "kb/utils/result.hpp"
#include "kb/utils/status.hpp"
#include "kb_test.hpp"

namespace {

kb::Result<uint16_t> Read(bool ready) noexcept {
  if (!ready) {
    return kb::Result<uint16_t>::Failure(kb::Status::NotReady, kb_test::Rt(7U));
  }
  return kb::Result<uint16_t>::Success(0x5A5AU);
}

constexpr kb::Result<int> kConstOk = kb::Result<int>::Success(3);
static_assert(kConstOk.IsOk() && (kConstOk.Value() == 3));
static_assert(!kb::Result<int>{}.IsOk(), "a default result is a failure");

} // namespace

KB_TEST("status: every value has its own name, unknown values have one too") {
  for (uint8_t i = 0U; i < kb::kStatusCount; ++i) {
    const auto s = static_cast<kb::Status>(i);
    KB_CHECK(std::string_view{kb::ToString(s)} != "Unknown");
    for (uint8_t j = 0U; j < i; ++j) {
      KB_CHECK(std::string_view{kb::ToString(s)} != kb::ToString(static_cast<kb::Status>(j)));
    }
  }
  KB_CHECK(std::string_view{kb::ToString(kb_test::Rt(static_cast<kb::Status>(200U)))} == "Unknown");
  KB_CHECK(std::string_view{kb::ToString(kb::Status::Timeout)} == "Timeout");
  KB_CHECK(kb::IsOk(kb::Status::Ok));
  KB_CHECK(!kb::IsOk(kb::Status::Fault));
}

KB_TEST("result: success holds the value and no detail") {
  const kb::Result<uint16_t> r = Read(true);
  KB_CHECK(r.IsOk());
  KB_CHECK_EQ(r.Error(), kb::Status::Ok);
  KB_CHECK_EQ(r.Value(), 0x5A5AU);
  KB_CHECK_EQ(r.ValueOr(1U), 0x5A5AU);
  KB_CHECK_EQ(r.Detail(), 0U);
  KB_CHECK_EQ(kb::ToStatus(r), kb::Status::Ok);
}

KB_TEST("result: failure carries status and detail, the value is the fallback") {
  const kb::Result<uint16_t> r = Read(false);
  KB_CHECK(!r.IsOk());
  KB_CHECK_EQ(r.Error(), kb::Status::NotReady);
  KB_CHECK_EQ(r.Detail(), 7U);
  KB_CHECK_EQ(r.ValueOr(9U), 9U);
  KB_CHECK_EQ(r.Value(), 0U);
}

KB_TEST("result: a failure that names Ok is recorded as Fault") {
  const auto r = kb::Result<int>::Failure(kb_test::Rt(kb::Status::Ok));
  KB_CHECK(!r.IsOk());
  KB_CHECK_EQ(r.Error(), kb::Status::Fault);
  const auto v = kb::Result<void>::Failure(kb_test::Rt(kb::Status::Ok), kb_test::Rt(3U));
  KB_CHECK(!v.IsOk());
  KB_CHECK_EQ(v.Error(), kb::Status::Fault);
  KB_CHECK_EQ(v.Detail(), 3U);
}

KB_TEST("result<void>: success, failure, from status and default") {
  const kb::Result<void> ok = kb::Result<void>::Success();
  KB_CHECK(ok.IsOk());
  KB_CHECK_EQ(ok.Detail(), 0U);
  const kb::Result<void> bad = kb::Result<void>::Failure(kb::Status::Timeout, kb_test::Rt(42U));
  KB_CHECK_EQ(bad.Error(), kb::Status::Timeout);
  KB_CHECK_EQ(bad.Detail(), 42U);
  KB_CHECK(kb::Result<void>::FromStatus(kb::Status::Ok).IsOk());
  KB_CHECK_EQ(kb::Result<void>::FromStatus(kb::Status::Busy).Error(), kb::Status::Busy);
  const kb::Result<void> none{};
  KB_CHECK_EQ(none.Error(), kb::Status::NotReady);
  KB_CHECK_EQ(kb::ToStatus(bad), kb::Status::Timeout);
}
