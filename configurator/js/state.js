// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
//
// The selection in the URL hash, so a link reproduces it. Only catalogue identifiers and the image name (kept to
// [a-z0-9-]) go in; nothing about the person using the page. Format (version 1):
//   #v=1&f=<family>&t=<target>&b=<board>&r=<rtos>&k=<kb-core version or empty>&p=<port version or empty>
//    &n=<image name>&d=<name>~<version>~<via>~<bus>,...&m=<bus>~<o|g>,...[&c=1 when release candidates are shown]
//    [&x=<C++ standard> when the image declares its language, docs/language-floor.md]
// The driver list's view rides along as optional parameters, written only when they differ from the defaults, and
// read only when `w=1` is present, so every older link keeps its meaning (version 1 of the view):
//   w=1 &q=<search text> &fb=<buses> &fc=<categories> &ft=<tags> (comma lists) &fw=0 (all drivers, not only those that
//   work with the target) &vw=l|g (list, grouped; cards when absent) &gb=b (group by bus; category when absent)
//   &so=c|b (sort by category or bus; name when absent; a leading - reverses) &su=1 (unavailable drivers listed)
(function (g) {
  'use strict';
  const KB = (g.KB = g.KB || {});
  const ID = /^[a-z0-9][a-z0-9_.-]*$/;
  const STANDARDS = [17, 20, 23, 26]; // resolver.mjs STANDARDS
  const VIEW_CODE = { list: 'l', grouped: 'g' };
  const SORT_CODE = { category: 'c', bus: 'b' };

  function clean(v) {
    return typeof v === 'string' && ID.test(v) ? v : '';
  }

  // The view's parameters (opts.view, a KB.picker view state); only those that differ from the defaults.
  function encodeView(p, view) {
    const v = KB.picker ? KB.picker.sanitize(view) : null;
    if (!v) return;
    const d = KB.picker.defaults();
    const out = [];
    if (v.q.trim()) out.push(['q', v.q.trim()]);
    for (const [key, list] of [['fb', v.bus], ['fc', v.cat], ['ft', v.tag]]) if (list.length) out.push([key, list.join(',')]);
    if (!v.fit) out.push(['fw', '0']);
    if (v.view !== d.view) out.push(['vw', VIEW_CODE[v.view]]);
    if (v.group !== d.group && v.group === 'bus') out.push(['gb', 'b']);
    if (v.sort !== d.sort || v.desc) out.push(['so', (v.desc ? '-' : '') + (SORT_CODE[v.sort] || 'n')]);
    if (v.unavail) out.push(['su', '1']);
    if (!out.length) return;
    p.set('w', '1');
    for (const [k, val] of out) p.set(k, val);
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
    if (o.view) encodeView(p, o.view);
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

  // The driver list's view from the hash, or null when the link carries none (then the stored choice and the defaults rule).
  function viewState(hash) {
    const p = new URLSearchParams(String(hash || '').replace(/^#/, ''));
    if (p.get('w') !== '1' || !KB.picker) return null;
    const list = (k) => (p.get(k) || '').split(',').filter(Boolean);
    const so = p.get('so') || '';
    const sortCode = so.replace(/^-/, '');
    return KB.picker.sanitize({
      q: p.get('q') || '', bus: list('fb'), cat: list('fc'), tag: list('ft'), fit: p.get('fw') !== '0',
      view: { l: 'list', g: 'grouped' }[p.get('vw')] || 'grid', group: p.get('gb') === 'b' ? 'bus' : 'category',
      sort: { c: 'category', b: 'bus' }[sortCode] || 'name', desc: so.startsWith('-'), unavail: p.get('su') === '1',
    });
  }

  KB.state = { encode, decode, flags, viewState };
})(typeof globalThis !== 'undefined' ? globalThis : this);
