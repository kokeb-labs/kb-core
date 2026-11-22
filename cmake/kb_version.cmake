# The one version of kb-core (ADR and docs/versioning.md). Every part's package carries it; nothing else sets it.
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
include_guard(GLOBAL)

set(KB_VERSION_MAJOR 0)
set(KB_VERSION_MINOR 1)
set(KB_VERSION_PATCH 0)
set(KB_VERSION "${KB_VERSION_MAJOR}.${KB_VERSION_MINOR}.${KB_VERSION_PATCH}")
