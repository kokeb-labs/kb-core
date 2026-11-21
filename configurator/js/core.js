// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
//
// Shared helpers of the configurator: versions, SHA-256, deterministic JSON and YAML scalars. Classic script (no
// module), so the page also runs when opened from a file; everything hangs off globalThis.KB.
(function (g) {
  'use strict';
  const KB = (g.KB = g.KB || {});

  // -- versions ------------------------------------------------------------------------------------------------
  const VERSION_RE = /^(\d+)\.(\d+)\.(\d+)(?:-rc\.(\d+))?$/;

  function parseVersion(v) {
    const m = VERSION_RE.exec(String(v || '').replace(/^v/, ''));
    if (!m) return null;
    return { major: +m[1], minor: +m[2], patch: +m[3], rc: m[4] === undefined ? null : +m[4] };
  }

  // Negative when a < b. A release sorts above every release candidate of the same version.
  function compareVersions(a, b) {
    const x = parseVersion(a);
    const y = parseVersion(b);
    if (!x || !y) throw new Error('not a version: ' + (x ? b : a));
    for (const k of ['major', 'minor', 'patch']) if (x[k] !== y[k]) return x[k] - y[k];
    if (x.rc === y.rc) return 0;
    if (x.rc === null) return 1;
    if (y.rc === null) return -1;
    return x.rc - y.rc;
  }

  // A range such as ">=0.4.0 <0.5.0" (space-separated comparators, all must hold). Empty means any.
  function satisfies(version, range) {
    const parts = String(range || '').trim().split(/\s+/).filter(Boolean);
    return parts.every((p) => {
      const m = /^(>=|<=|>|<|=)?(.+)$/.exec(p);
      const c = compareVersions(version, m[2]);
      switch (m[1] || '=') {
        case '>=': return c >= 0;
        case '<=': return c <= 0;
        case '>': return c > 0;
        case '<': return c < 0;
        default: return c === 0;
      }
    });
  }

  // -- SHA-256 (synchronous, so every output is computed the same way in the page and in the tests) -----------
  const K = new Uint32Array([
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
  ]);

  function utf8(s) {
    return new TextEncoder().encode(s);
  }

  function sha256(input) {
    const data = typeof input === 'string' ? utf8(input) : input;
    const bitLen = data.length * 8;
    const padded = new Uint8Array(((data.length + 9 + 63) >> 6) << 6);
    padded.set(data);
    padded[data.length] = 0x80;
    const view = new DataView(padded.buffer);
    view.setUint32(padded.length - 8, Math.floor(bitLen / 0x100000000));
    view.setUint32(padded.length - 4, bitLen >>> 0);
    const h = new Uint32Array([0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19]);
    const w = new Uint32Array(64);
    const rotr = (x, n) => (x >>> n) | (x << (32 - n));
    for (let off = 0; off < padded.length; off += 64) {
      for (let i = 0; i < 16; i++) w[i] = view.getUint32(off + i * 4);
      for (let i = 16; i < 64; i++) {
        const s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >>> 3);
        const s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >>> 10);
        w[i] = (w[i - 16] + s0 + w[i - 7] + s1) >>> 0;
      }
      let [a, b, c, d, e, f, gg, hh] = h;
      for (let i = 0; i < 64; i++) {
        const S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const ch = (e & f) ^ (~e & gg);
        const t1 = (hh + S1 + ch + K[i] + w[i]) >>> 0;
        const S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const maj = (a & b) ^ (a & c) ^ (b & c);
        const t2 = (S0 + maj) >>> 0;
        hh = gg; gg = f; f = e; e = (d + t1) >>> 0; d = c; c = b; b = a; a = (t1 + t2) >>> 0;
      }
      h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += gg; h[7] += hh;
    }
    return Array.from(h, (x) => x.toString(16).padStart(8, '0')).join('');
  }

  // SHA-1, only for name-based UUIDs (RFC 9562 version 5), which are defined over SHA-1.
  function sha1Bytes(data) {
    const bitLen = data.length * 8;
    const padded = new Uint8Array(((data.length + 9 + 63) >> 6) << 6);
    padded.set(data);
    padded[data.length] = 0x80;
    const view = new DataView(padded.buffer);
    view.setUint32(padded.length - 8, Math.floor(bitLen / 0x100000000));
    view.setUint32(padded.length - 4, bitLen >>> 0);
    const h = [0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0];
    const w = new Uint32Array(80);
    const rotl = (x, n) => (x << n) | (x >>> (32 - n));
    for (let off = 0; off < padded.length; off += 64) {
      for (let i = 0; i < 16; i++) w[i] = view.getUint32(off + i * 4);
      for (let i = 16; i < 80; i++) w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
      let [a, b, c, d, e] = h;
      for (let i = 0; i < 80; i++) {
        const f = i < 20 ? (b & c) | (~b & d) : i < 40 ? b ^ c ^ d : i < 60 ? (b & c) | (b & d) | (c & d) : b ^ c ^ d;
        const k = i < 20 ? 0x5a827999 : i < 40 ? 0x6ed9eba1 : i < 60 ? 0x8f1bbcdc : 0xca62c1d6;
        const t = (rotl(a, 5) + f + e + k + w[i]) >>> 0;
        e = d; d = c; c = rotl(b, 30) >>> 0; b = a; a = t;
      }
      h[0] = (h[0] + a) >>> 0; h[1] = (h[1] + b) >>> 0; h[2] = (h[2] + c) >>> 0; h[3] = (h[3] + d) >>> 0; h[4] = (h[4] + e) >>> 0;
    }
    const out = new Uint8Array(20);
    const ov = new DataView(out.buffer);
    h.forEach((x, i) => ov.setUint32(i * 4, x));
    return out;
  }

  const NAMESPACE_URL = '6ba7b811-9dad-11d1-80b4-00c04fd430c8';

  // uuid.uuid5(uuid.NAMESPACE_URL, name) of Python, byte for byte.
  function uuid5(name, namespace) {
    const ns = (namespace || NAMESPACE_URL).replace(/-/g, '');
    const nsBytes = new Uint8Array(16);
    for (let i = 0; i < 16; i++) nsBytes[i] = parseInt(ns.slice(i * 2, i * 2 + 2), 16);
    const nameBytes = utf8(name);
    const all = new Uint8Array(16 + nameBytes.length);
    all.set(nsBytes);
    all.set(nameBytes, 16);
    const hash = sha1Bytes(all).slice(0, 16);
    hash[6] = (hash[6] & 0x0f) | 0x50;
    hash[8] = (hash[8] & 0x3f) | 0x80;
    const hex = Array.from(hash, (x) => x.toString(16).padStart(2, '0')).join('');
    return `${hex.slice(0, 8)}-${hex.slice(8, 12)}-${hex.slice(12, 16)}-${hex.slice(16, 20)}-${hex.slice(20)}`;
  }

  // -- deterministic text -----------------------------------------------------------------------------------------
  function sortKeys(value) {
    if (Array.isArray(value)) return value.map(sortKeys);
    if (value && typeof value === 'object') {
      const out = {};
      for (const k of Object.keys(value).sort()) out[k] = sortKeys(value[k]);
      return out;
    }
    return value;
  }

  // JSON with sorted keys, two-space indent and a final newline: the same selection gives the same bytes.
  function stableJson(value) {
    return JSON.stringify(sortKeys(value), null, 2) + '\n';
  }

  // A YAML scalar that reads back as the same string (quoted when plain style would change its meaning).
  function yamlScalar(v) {
    if (v === null || v === undefined) return 'null';
    if (typeof v === 'boolean' || typeof v === 'number') return String(v);
    const s = String(v);
    if (s === '' || /^[\s]|[\s]$|^[-?:,\[\]{}#&*!|>'"%@`]|: | #|[,\[\]{}]/.test(s) ||
        /^(true|false|null|yes|no|on|off|~)$/i.test(s) || /^[-+]?(\d|\.\d)/.test(s) && !/^\d+\.\d+\.\d+/.test(s)) {
      return JSON.stringify(s);
    }
    return s;
  }

  function yamlFlow(obj) {
    return '{' + Object.keys(obj).map((k) => k + ': ' + (Array.isArray(obj[k])
      ? '[' + obj[k].map(yamlScalar).join(', ') + ']' : yamlScalar(obj[k]))).join(', ') + '}';
  }

  // Image names are kept to a safe alphabet: they become folder, preset and CMake names.
  function safeName(s, fallback) {
    const out = String(s || '').toLowerCase().replace(/[^a-z0-9-]+/g, '-').replace(/^-+|-+$/g, '').slice(0, 40);
    return out || fallback || 'kb-image';
  }

  KB.core = { parseVersion, compareVersions, satisfies, sha256, uuid5, utf8, sortKeys, stableJson, yamlScalar, yamlFlow, safeName };
})(typeof globalThis !== 'undefined' ? globalThis : this);
