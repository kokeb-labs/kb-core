// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
//
// "Open an existing project": reads a project's kb-image.yml and kb-image.lock.yml that the user picks, in this
// browser only (nothing is uploaded, nothing is stored), and turns them into the page's selection. The lock is
// JSON after one comment line; kb-image.yml is read by a small YAML subset parser (block mappings and sequences,
// flow mappings and sequences, plain and quoted scalars, comments), which refuses anchors, aliases, tags,
// multi-line strings and several documents. Plain scalars resolve as PyYAML (YAML 1.1) resolves them for the forms
// it accepts (decimal integers and floats, booleans, null); forms PyYAML would turn into something else (octal, hex,
// binary, sexagesimal, infinity and NaN, dates, non-string keys) are refused with "quote it", so `kb upgrade` and
// the page never read the same file differently. Work is linear in the file size, and the file and its lines are
// capped, so a crafted file cannot hold up the page.
(function (g) {
  'use strict';
  const KB = (g.KB = g.KB || {});

  const MAX_BYTES = 256 * 1024;
  const MAX_DEPTH = 24;
  const MAX_LINES = 10000;      // a kb-image.yml has tens of lines
  const MAX_FLOW_LINES = 500;   // lines one flow collection may span

  class YamlError extends Error {}

  function fail(line, msg) {
    throw new YamlError(line ? `line ${line}: ${msg}` : msg);
  }

  // Cut a trailing comment: a '#' at the start or after a blank, outside quotes.
  function stripComment(text, lineNo) {
    let quote = null;
    let closed = -2; // index of the last closing quote: PyYAML also ends a quoted scalar at "#" right after it
    for (let i = 0; i < text.length; i++) {
      const ch = text[i];
      if (quote) {
        if (quote === '"' && ch === '\\') i++;
        else if (ch === quote) {
          if (quote === "'" && text[i + 1] === "'") i++;
          else { quote = null; closed = i; }
        }
      } else if (ch === '"' || ch === "'") {
        const prev = i === 0 ? ' ' : text[i - 1];
        if (/[\s[{,:]/.test(prev) || i === 0) quote = ch;
      } else if (ch === '#' && (i === 0 || /\s/.test(text[i - 1]) || closed === i - 1)) {
        return text.slice(0, i).replace(/\s+$/, '');
      }
    }
    if (quote) fail(lineNo, 'a quoted string is not closed on its line');
    return text.replace(/\s+$/, '');
  }

  // Plain forms PyYAML resolves to something this reader does not produce: refused so both read a file alike.
  const OTHER_TYPES = [
    [/^[-+]?0b[0-1_]+$/, 'a binary number'],
    [/^[-+]?0[0-7_]+$/, 'an octal number'],
    [/^[-+]?0x[0-9a-fA-F_]+$/, 'a hexadecimal number'],
    [/^[-+]?[0-9][0-9_]*(:[0-5]?[0-9])+(\.[0-9_]*)?$/, 'a base-60 number'],
    [/^([-+]?\.(inf|Inf|INF)|\.(nan|NaN|NAN))$/, 'infinity or NaN'],
    [/^[0-9]{4}-[0-9]{1,2}-[0-9]{1,2}(([Tt]|[ \t]+)[0-9].*)?$/, 'a date'],
    [/^(=|<<)$/, 'a YAML value or merge key'],
  ];

  // The common case, a name or version that is neither a keyword nor a number: a string, without the checks below.
  const PLAIN_NAME = /^[A-Za-z_][A-Za-z0-9_.-]*$/;
  const KEYWORD = /^(null|Null|NULL|yes|Yes|YES|true|True|TRUE|on|On|ON|no|No|NO|false|False|FALSE|off|Off|OFF)$/;

  function plainScalar(s, lineNo) {
    if (PLAIN_NAME.test(s) && !KEYWORD.test(s)) return s;
    for (const [re, what] of OTHER_TYPES) if (re.test(s)) fail(lineNo, `${s.slice(0, 30)} reads as ${what} in YAML 1.1; quote it`);
    if (s === '' || s === '~' || /^(null|Null|NULL)$/.test(s)) return null;
    if (/^(yes|Yes|YES|true|True|TRUE|on|On|ON)$/.test(s)) return true;
    if (/^(no|No|NO|false|False|FALSE|off|Off|OFF)$/.test(s)) return false;
    if (/^[-+]?(0|[1-9][0-9_]*)$/.test(s)) return Number(s.replace(/_/g, ''));
    if (/^([-+]?[0-9][0-9_]*\.[0-9_]*([eE][-+][0-9]+)?|\.[0-9_]+([eE][-+][0-9]+)?)$/.test(s)) return Number(s.replace(/_/g, ''));
    if (/:(\s|$)/.test(s)) fail(lineNo, `a plain value cannot hold ": "; quote it: ${s.slice(0, 30)}`);
    if (/^[&*!|>%@`]/.test(s)) fail(lineNo, `anchors, aliases, tags and block strings are not supported: ${s.slice(0, 20)}`);
    return s;
  }

  function quoted(s, lineNo) {
    if (s[0] === '"') {
      try { return JSON.parse(s.replace(/\\\//g, '/')); } catch (e) { fail(lineNo, `bad double-quoted string ${s.slice(0, 30)}`); }
    }
    if (!s.endsWith("'") || s.length < 2) fail(lineNo, `bad single-quoted string ${s.slice(0, 30)}`);
    return s.slice(1, -1).replace(/''/g, "'");
  }

  // A mapping key: a string, never __proto__ (it would set the object's prototype), never a number, boolean or null
  // (PyYAML would give a non-string key that kb refuses).
  function keyOf(tok, lineNo) {
    const k = tok.quoted ? quoted(tok.text, lineNo) : plainScalar(tok.text, lineNo);
    if (typeof k !== 'string') fail(lineNo, `the key ${tok.text.slice(0, 20) || '(empty)'} is not a string; quote it`);
    if (k === '__proto__') fail(lineNo, 'the key __proto__ is not allowed');
    return k;
  }

  function scalar(s, lineNo) {
    const t = s.trim();
    if (t[0] === '"' || t[0] === "'") return quoted(t, lineNo);
    return plainScalar(t, lineNo);
  }

  // Flow collections: {a: b, c: [d, e]} and [a, {b: c}].
  function parseFlow(text, lineNo, depth) {
    let i = 0;
    const ws = () => { while (i < text.length && (text[i] === ' ' || text[i] === '\t')) i++; };
    function token(stops) {
      ws();
      const start = i;
      if (text[i] === '"' || text[i] === "'") {
        const q = text[i++];
        while (i < text.length) {
          if (q === '"' && text[i] === '\\') { i += 2; continue; }
          if (text[i] === q) {
            if (q === "'" && text[i + 1] === "'") { i += 2; continue; }
            break;
          }
          i++;
        }
        if (i >= text.length) fail(lineNo, 'a quoted string is not closed');
        i++;
        return { quoted: true, text: text.slice(start, i) };
      }
      while (i < text.length && !stops.includes(text[i]) && !(text[i] === ':' && /[\s,\]}]/.test(text[i + 1] || ' '))) i++;
      return { quoted: false, text: text.slice(start, i).trim() };
    }
    function value(d) {
      if (d > MAX_DEPTH) fail(lineNo, 'nested too deeply');
      ws();
      if (text[i] === '{') {
        i++;
        const out = {};
        ws();
        if (text[i] === '}') { i++; return out; }
        for (;;) {
          const key = keyOf(token(',:}'), lineNo);
          ws();
          let v = null;
          if (text[i] === ':') { i++; ws(); v = (text[i] === ',' || text[i] === '}') ? null : value(d + 1); }
          if (Object.prototype.hasOwnProperty.call(out, key)) fail(lineNo, `duplicate key ${key}`);
          out[key] = v;
          ws();
          if (text[i] === ',') { i++; ws(); if (text[i] === '}') { i++; return out; } continue; }
          if (text[i] === '}') { i++; return out; }
          fail(lineNo, 'expected , or } in a flow mapping');
        }
      }
      if (text[i] === '[') {
        i++;
        const out = [];
        ws();
        if (text[i] === ']') { i++; return out; }
        for (;;) {
          ws();
          const start = i;
          const tok = (text[i] === '{' || text[i] === '[') ? null : token(',]');
          let v;
          if (tok === null) v = value(d + 1);
          else {
            ws();
            if (text[i] === ':') { // a single-pair mapping inside a sequence: [a: b]
              i = start;
              const k = token(':');
              i++;
              v = {};
              v[keyOf(k, lineNo)] = value(d + 1);
            } else v = tok.quoted ? quoted(tok.text, lineNo) : plainScalar(tok.text, lineNo);
          }
          out.push(v);
          ws();
          if (text[i] === ',') { i++; ws(); if (text[i] === ']') { i++; return out; } continue; }
          if (text[i] === ']') { i++; return out; }
          fail(lineNo, 'expected , or ] in a flow sequence');
        }
      }
      const tok = token(',]}');
      return tok.quoted ? quoted(tok.text, lineNo) : plainScalar(tok.text, lineNo);
    }
    const v = value(depth);
    ws();
    if (i !== text.length) fail(lineNo, `unexpected text after a flow value: ${text.slice(i, i + 20)}`);
    return v;
  }

  // Bracket depth and open quote of flow text fed line by line: each line is scanned once (linear in the file).
  function flowScanner() {
    let depth = 0;
    let quote = null;
    return (text) => {
      for (let i = 0; i < text.length; i++) {
        const ch = text[i];
        if (quote) { if (quote === '"' && ch === '\\') i++; else if (ch === quote) quote = null; continue; }
        if (ch === '"' || ch === "'") quote = ch;
        else if (ch === '{' || ch === '[') depth++;
        else if (ch === '}' || ch === ']') depth--;
      }
      return depth <= 0;
    };
  }

  function parseYaml(text) {
    if (typeof text !== 'string') fail(0, 'not text');
    if (text.length > MAX_BYTES) fail(0, 'the file is larger than 256 KiB');
    const raw = text.replace(/^﻿/, '').split(/\r?\n/);
    const lines = [];
    let docs = 0;
    for (let n = 0; n < raw.length; n++) {
      const src = raw[n];
      if (/^\t| \t/.test(src.match(/^[ \t]*/)[0])) fail(n + 1, 'tabs are not allowed for indentation');
      if (/^---(\s|$)/.test(src)) { if (++docs > 1 || lines.length) fail(n + 1, 'only one YAML document is supported'); continue; }
      if (/^\.\.\.(\s|$)/.test(src)) {
        for (let m = n + 1; m < raw.length; m++) if (stripComment(raw[m], m + 1).trim()) fail(m + 1, 'text after the end of the document (...)');
        break;
      }
      if (/^%/.test(src)) fail(n + 1, 'YAML directives are not supported');
      const body = stripComment(src, n + 1);
      if (!body.trim()) continue;
      lines.push({ n: n + 1, indent: body.match(/^ */)[0].length, text: body.trim() });
      if (lines.length > MAX_LINES) fail(n + 1, `more than ${MAX_LINES} lines: not a kb-image.yml`);
    }
    let pos = 0;

    function flowValue(first, lineNo, depth) {
      const feed = flowScanner();
      const parts = [first];
      let closed = feed(first);
      while (!closed) {
        if (pos >= lines.length) fail(lineNo, 'a flow collection is not closed');
        if (parts.length >= MAX_FLOW_LINES) fail(lineNo, `a flow collection spans more than ${MAX_FLOW_LINES} lines`);
        const next = lines[pos++].text;
        parts.push(next);
        closed = feed(' ' + next);
      }
      return parseFlow(parts.join(' '), lineNo, depth);
    }

    function inlineValue(rest, lineNo, indent, depth) {
      if (rest === '') {
        if (pos < lines.length && lines[pos].indent > indent) return block(lines[pos].indent, depth + 1);
        if (pos < lines.length && lines[pos].indent === indent && lines[pos].text.startsWith('- ')) return block(indent, depth + 1);
        if (pos < lines.length && lines[pos].indent === indent && lines[pos].text === '-') return block(indent, depth + 1);
        return null;
      }
      if (rest[0] === '{' || rest[0] === '[') return flowValue(rest, lineNo, depth);
      if (/^[|>]/.test(rest)) fail(lineNo, 'block strings (| and >) are not supported');
      return scalar(rest, lineNo);
    }

    function splitKey(text, lineNo) {
      let k;
      let rest;
      if (text[0] === '"' || text[0] === "'") {
        const q = text[0];
        let i = 1;
        while (i < text.length && text[i] !== q) { if (q === '"' && text[i] === '\\') i++; i++; }
        rest = text.slice(i + 1);
        if (!/^:(\s|$)/.test(rest)) return null;
        if (/^:[ ]*\t/.test(rest)) fail(lineNo, 'a tab after ":" is not allowed');
        k = keyOf({ quoted: true, text: text.slice(0, i + 1) }, lineNo);
        return [k, rest.slice(1).trim()];
      }
      const m = /^([^\s:#][^:]*?|[^\s:#]):(\s+|$)(.*)$/.exec(text);
      if (!m) return null;
      if (/^[[{&*!|>]/.test(m[1])) return null;
      if (m[2].includes('\t')) fail(lineNo, 'a tab after ":" is not allowed');
      return [keyOf({ quoted: false, text: m[1] }, lineNo), m[3]];
    }

    function block(indent, depth) {
      if (depth > MAX_DEPTH) fail(lines[pos] ? lines[pos].n : 0, 'nested too deeply');
      const first = lines[pos];
      if (first.text === '-' || first.text.startsWith('- ')) {
        const out = [];
        while (pos < lines.length && lines[pos].indent === indent && (lines[pos].text === '-' || lines[pos].text.startsWith('- '))) {
          const ln = lines[pos++];
          const rest = ln.text === '-' ? '' : ln.text.slice(2).trim();
          const inner = indent + 2 + (ln.text.length > 2 ? ln.text.slice(2).match(/^ */)[0].length : 0);
          const kv = rest && rest[0] !== '{' && rest[0] !== '[' ? splitKey(rest, ln.n) : null;
          if (kv) {
            // "- key: value" starts a mapping at the item's indentation
            // (the item's own slot is reused for it: no array copy, so long sequences stay linear)
            pos -= 1;
            lines[pos] = { n: ln.n, indent: inner, text: rest };
            out.push(block(inner, depth + 1));
          } else out.push(inlineValue(rest, ln.n, indent, depth));
        }
        if (pos < lines.length && lines[pos].indent > indent) fail(lines[pos].n, 'unexpected indentation');
        return out;
      }
      if ((first.text[0] === '{' || first.text[0] === '[') && !splitKey(first.text, first.n)) {
        pos++;
        return flowValue(first.text, first.n, depth);
      }
      const out = {};
      while (pos < lines.length && lines[pos].indent === indent) {
        const ln = lines[pos];
        if (ln.text.startsWith('- ') || ln.text === '-') break;
        const kv = splitKey(ln.text, ln.n);
        if (!kv) fail(ln.n, `expected "key: value", found ${ln.text.slice(0, 30)}`);
        pos++;
        if (Object.prototype.hasOwnProperty.call(out, kv[0])) fail(ln.n, `duplicate key ${kv[0]}`);
        out[kv[0]] = inlineValue(kv[1], ln.n, indent, depth);
      }
      if (pos < lines.length && lines[pos].indent > indent) fail(lines[pos].n, 'unexpected indentation');
      return out;
    }

    if (!lines.length) return null;
    const doc = lines.length === 1 && !splitKey(lines[0].text, lines[0].n) && !lines[0].text.startsWith('- ')
      ? (pos++, inlineValue(lines[0].text, lines[0].n, 0, 0))
      : block(lines[0].indent, 0);
    if (pos < lines.length) fail(lines[pos].n, 'unexpected text after the document');
    return doc;
  }

  // A lock is one comment line and canonical JSON; anything else that starts with { is JSON too.
  function parseDocument(text) {
    const body = String(text).replace(/^﻿/, '').split(/\r?\n/);
    let i = 0;
    while (i < body.length && (!body[i].trim() || /^\s*#/.test(body[i]))) i++;
    const rest = body.slice(i).join('\n');
    if (/^\s*[{[]/.test(rest)) {
      try { return JSON.parse(rest); } catch (e) { /* not JSON: a flow-style YAML document */ }
    }
    return parseYaml(String(text));
  }

  function isObj(v) {
    return v !== null && typeof v === 'object' && !Array.isArray(v);
  }

  // Which of the picked files is the image and which the lock.
  function classify(files) {
    const out = { image: null, lock: null, problems: [] };
    for (const f of files) {
      let doc;
      try {
        doc = parseDocument(f.text);
      } catch (e) {
        out.problems.push(`${f.name}: ${e.message}`);
        continue;
      }
      const isLock = isObj(doc) && Array.isArray(doc.ports) && isObj(doc.kb_core) && typeof doc.kb_core.commit === 'string';
      const slot = isLock ? 'lock' : isObj(doc) && isObj(doc.mcu) ? 'image' : null;
      if (!slot) { out.problems.push(`${f.name}: neither a kb-image.yml nor a kb-image.lock.yml`); continue; }
      if (out[slot]) { out.problems.push(`${f.name}: a second ${slot === 'lock' ? 'lock' : 'image'}; the first one is used`); continue; }
      out[slot] = { name: f.name, doc };
    }
    return out;
  }

  const NAME_KIND = /^(i2c|spi|uart|can|adc|pio|gpio)[0-9]*$/;
  const BUS_KINDS = ['adc', 'can', 'canopen', 'gpio', 'i2c', 'modbus', 'onewire', 'pio', 'spi', 'uart'];

  // The image a lock describes, for a project that has only its lock.
  function imageFromLock(lock) {
    const mcu = isObj(lock.mcu) ? lock.mcu : {};
    const image = { schema: 1, image: lock.image, kb_core: { version: lock.kb_core.version },
      mcu: { family: mcu.family, target: mcu.target } };
    const port = (Array.isArray(lock.ports) ? lock.ports : [])[0];
    if (isObj(port) && port.in_tree === false && typeof port.version === 'string') image.mcu.port = { version: port.version };
    if (lock.rtos) image.rtos = lock.rtos;
    image.drivers = (Array.isArray(lock.drivers) ? lock.drivers : []).filter(isObj).map((d) => ({ name: d.name, version: d.version }));
    image.buses = {};
    for (const b of Array.isArray(lock.buses) ? lock.buses : []) {
      if (!isObj(b) || typeof b.name !== 'string') continue;
      const bus = { model: b.model, devices: Array.isArray(b.devices) ? b.devices.filter((x) => typeof x === 'string') : [] };
      // An explicit kind only when the name does not already say it (i2c0 is i2c; modbus on uart1 needs kind).
      const named = NAME_KIND.exec(b.name);
      if (typeof b.kind === 'string' && BUS_KINDS.includes(b.kind) && !(named && named[1] === b.kind)) bus.kind = b.kind;
      image.buses[b.name] = bus;
    }
    return image;
  }

  const BUS_ID = /^[a-z][a-z0-9_]{0,31}$/;
  const VERSION_ID = /^[0-9]+\.[0-9]+\.[0-9]+(-rc\.[0-9]+)?$/;
  const str = (v) => (typeof v === 'string' ? v : '');

  // The page's selection of a kb image (and its lock, which fixes every version the image leaves out).
  function toSelection(site, image, lock) {
    const notes = [];
    const lockVersions = new Map((lock && Array.isArray(lock.drivers) ? lock.drivers : []).filter(isObj).map((d) => [d.name, d.version]));
    const mcu = isObj(image.mcu) ? image.mcu : {};
    const kb = isObj(image.kb_core) && typeof image.kb_core.version === 'string' ? image.kb_core.version
      : lock && lock.kb_core ? lock.kb_core.version : null;
    const port = isObj(mcu.port) && typeof mcu.port.version === 'string' ? mcu.port.version : null;
    const buses = isObj(image.buses) ? image.buses : {};
    const busOf = new Map();
    for (const id of Object.keys(buses).sort()) {
      const b = buses[id];
      if (!BUS_ID.test(id)) { notes.push('a bus with a name outside [a-z][a-z0-9_]* was left out'); continue; }
      for (const dev of isObj(b) && Array.isArray(b.devices) ? b.devices : []) if (!busOf.has(dev)) busOf.set(dev, id);
    }
    const sel = {
      image: KB.core.safeName(str(image.image) || (lock && lock.image) || 'kb-image'),
      kb_core: VERSION_ID.test(kb || '') ? kb : null,
      port: VERSION_ID.test(port || '') ? port : null,
      mcu: { family: str(mcu.family), target: str(mcu.target), board: null },
      rtos: str(image.rtos),
      drivers: [],
      buses: {},
    };
    for (const item of Array.isArray(image.drivers) ? image.drivers : []) {
      const name = typeof item === 'string' ? item : isObj(item) ? str(item.name) : '';
      if (!name) { notes.push('a driver entry without a name was left out'); continue; }
      // Only catalogue drivers enter the selection (and so the address bar); the others are named in the notes.
      if (!site.drivers.some((d) => d.name === name)) { notes.push(`${name} is not in this catalogue and was left out`); continue; }
      const version = isObj(item) && typeof item.version === 'string' ? item.version : lockVersions.get(name) || '';
      const bus = busOf.get(name) || '';
      const b = bus ? buses[bus] : null;
      const m = NAME_KIND.exec(bus);
      const via = b && BUS_KINDS.includes(b.kind) ? b.kind : m ? m[1] : '';
      sel.drivers.push({ name, version: VERSION_ID.test(version) ? version : '', via, bus });
    }
    sel.drivers.sort((a, b) => (a.name < b.name ? -1 : 1));
    for (const id of Object.keys(buses).sort()) {
      const b = buses[id];
      if (BUS_ID.test(id) && isObj(b) && (b.model === 'owner' || b.model === 'guarded')) sel.buses[id] = { model: b.model };
    }
    return { selection: sel, notes };
  }

  KB.project = { parseYaml, parseDocument, classify, imageFromLock, toSelection, YamlError };
})(typeof globalThis !== 'undefined' ? globalThis : this);
