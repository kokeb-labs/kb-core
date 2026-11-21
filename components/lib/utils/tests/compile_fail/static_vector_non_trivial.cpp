// A fixed-capacity container holds trivially copyable value types only.
// EXPECT: (constraints not satisfied|constraint|InlineElement|template constraint failure)
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include "kb/utils/static_vector.hpp"

struct Owning {
  Owning() = default;
  Owning(const Owning&) {}
  Owning& operator=(const Owning&) {
    return *this;
  }
};
kb::utils::StaticVector<Owning, 2> v{};
