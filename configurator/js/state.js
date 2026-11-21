// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
//
// The selection in the URL hash, so a link reproduces it. Only catalogue identifiers and the image name (kept to
// [a-z0-9-]) go in; nothing about the person using the page. Format (version 1):
//   #v=1&f=<family>&t=<target>&b=<board>&r=<rtos>&k=<kb-core version or empty>&p=<port version or empty>
//    &n=<image name>&d=<name>~<version>~<via>~<bus>,...&m=<bus>~<o|g>,...[&c=1 when release candidates are shown]
//    [&x=<C++ standard> when the image declares its language, docs/language-floor.md]
(function (g) {
  'use strict';
  const KB = (g.KB = g.KB || {});
  const ID = /^[a-z0-9][a-z0-9_.-]*$/;
  const STANDARDS = [17, 20, 23, 26]; // resolver.mjs STANDARDS

  function clean(v) {
    return typeof v === 'string' && ID.test(v) ? v : '';
  }

  // opts.rc adds c=1 (release candidates shown); opts.omitName leaves the image name out (an opened project's own name).
  function encode(image, opts) {
    const o = opts || {};
    const p = new URLSearchParams();
    p.set('v', '1');
    p.set('f', image.mcu.family || '');
    p.set('t', image.mcu.target || '');
    p.set('b', image.mcu.board || '');
    p.set('r', image.rtos || '');
    p.set('k', image.kb_core || '');
    p.set('p', image.port || '');
    if (!o.omitName) p.set('n', KB.core.safeName(image.image));
    p.set('d', (image.drivers || []).map((d) => [d.name, d.version, d.via, d.bus].join('~')).join(','));
    p.set('m', Object.keys(image.buses || {}).sort().map((id) => `${id}~${image.buses[id].model === 'owner' ? 'o' : 'g'}`).join(','));
    if (image.cxx) p.set('x', String(image.cxx));
    if (o.rc) p.set('c', '1');
    return '#' + p.toString().replace(/%7E/g, '~').replace(/%2C/g, ',');
  }

  function decode(hash) {
    const p = new URLSearchParams(String(hash || '').replace(/^#/, ''));
    if (p.get('v') !== '1') return null;
    const drivers = (p.get('d') || '').split(',').filter(Boolean).map((s) => {
      const [name, version, via, bus] = s.split('~');
      return { name: clean(name), version: clean(version), via: clean(via), bus: clean(bus) };
    }).filter((d) => d.name);
    const buses = {};
    for (const s of (p.get('m') || '').split(',').filter(Boolean)) {
      const [id, m] = s.split('~');
      if (clean(id)) buses[id] = { model: m === 'o' ? 'owner' : 'guarded' };
    }
    const out = {
      image: KB.core.safeName(p.get('n')),
      kb_core: clean(p.get('k')) || null,
      port: clean(p.get('p')) || null,
      mcu: { family: clean(p.get('f')), target: clean(p.get('t')), board: clean(p.get('b')) || null },
      rtos: clean(p.get('r')),
      drivers,
      buses,
    };
    if (STANDARDS.includes(Number(p.get('x')))) out.cxx = Number(p.get('x')); // only when the link carries one
    return out;
  }

  // Page switches carried in the hash beside the selection.
  function flags(hash) {
    const p = new URLSearchParams(String(hash || '').replace(/^#/, ''));
    return { rc: p.get('c') === '1' };
  }

  KB.state = { encode, decode, flags };
})(typeof globalThis !== 'undefined' ? globalThis : this);
