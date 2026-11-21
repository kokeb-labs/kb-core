// A fixed string needs room for at least one character.
// EXPECT: (constraints not satisfied|constraint|template constraint failure)
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include "kb/utils/fixed_string.hpp"

kb::utils::FixedString<0> s{};
