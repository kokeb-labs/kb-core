// A factory's result may not be discarded ([[nodiscard]] is an error with -Werror).
// EXPECT: (nodiscard|ignoring return value|unused-result)
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include "kb/utils/result.hpp"

void Use() {
  kb::Result<void>::Success();
}
