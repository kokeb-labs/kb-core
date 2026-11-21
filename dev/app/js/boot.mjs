// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
//
// Loads the shared resolver (resolver.mjs, an ES module, integrity-checked through its modulepreload link) and
// refreshes the page with it. offline.html carries the same code inline instead.
import * as engine from '../resolver.mjs';

globalThis.KB.engine = engine;
globalThis.KB.ui.refresh();
