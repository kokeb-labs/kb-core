// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
//
// Loads the page's classic scripts into a fresh context, as the browser does, so the tests run the same code, and
// attaches the shared resolver module (resolver.mjs) as KB.engine, as js/boot.mjs does in the page.
import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import vm from 'node:vm';

export const root = join(dirname(fileURLToPath(import.meta.url)), '..');

export async function load(withEngine = true) {
  const host = new TextEncoder();
  const context = vm.createContext({ URLSearchParams, console, hostEncode: (s) => host.encode(s) });
  context.globalThis = context;
  // TextEncoder is not an ECMAScript built-in; give the context one that returns the context's own Uint8Array,
  // as a browser would (the vendored ZIP library checks instanceof Uint8Array).
  vm.runInContext('globalThis.TextEncoder = class { encode(s) { return new Uint8Array(hostEncode(s)); } };', context);
  for (const f of ['vendor/fflate-0.8.2/fflate.umd.js', 'data/catalog.js', 'js/core.js', 'js/resolver.js', 'js/generate.js', 'js/picker.js', 'js/state.js', 'js/shared.js', 'js/project.js', 'data/evidence.js', 'js/views.js', 'data/vulnerabilities.js', 'js/panel.js']) {
    vm.runInContext(readFileSync(join(root, f), 'utf8'), context, { filename: f });
  }
  if (withEngine) context.KB.engine = await import(pathToFileURL(join(root, 'resolver.mjs')).href);
  return context;
}
