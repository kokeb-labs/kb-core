// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
//
// The driver picker's logic without any page code: filtering, sorting, grouping and chunking of the catalogue, the
// view state that goes in the address bar, and the cascade (which choices a change to the target, the RTOS or the C++
// standard takes away, and why). Every rule about what a target allows comes from the catalogue through
// KB.engine.availability(); nothing here knows a driver or a target by name.
(function (g) {
  'use strict';
  const KB = (g.KB = g.KB || {});

  const VIEWS = ['grid', 'list', 'grouped'];
  const GROUPS = ['category', 'bus'];
  const SORTS = ['name', 'category', 'bus'];
  const CHUNK = 24; // items shown at first, and added by each "Show more"
  const PAGED_ABOVE = 60; // a result of up to this many is shown whole
  const MAX_TEXT = 40;
  const ID = /^[a-z0-9][a-z0-9_.-]*$/;

  /** The view a person starts with: every driver that works with the target, as cards, by name. */
  function defaults() {
    return { q: '', bus: [], cat: [], tag: [], fit: true, view: 'grid', group: 'category', sort: 'name', desc: false, unavail: false };
  }

  function ids(list) {
    return [...new Set((Array.isArray(list) ? list : []).filter((s) => typeof s === 'string' && ID.test(s)))].sort();
  }

  /** A view state made safe: unknown values fall back to the defaults, lists keep catalogue-style identifiers only. */
  function sanitize(v) {
    const d = defaults();
    const x = v && typeof v === 'object' ? v : {};
    return {
      q: typeof x.q === 'string' ? x.q.toLowerCase().replace(/[^a-z0-9 ._-]/g, '').replace(/\s+/g, ' ').trimStart().slice(0, MAX_TEXT) : d.q,
      bus: ids(x.bus), cat: ids(x.cat), tag: ids(x.tag),
      fit: x.fit === false ? false : d.fit,
      view: VIEWS.includes(x.view) ? x.view : d.view,
      group: GROUPS.includes(x.group) ? x.group : d.group,
      sort: SORTS.includes(x.sort) ? x.sort : d.sort,
      desc: x.desc === true,
      unavail: x.unavail === true,
    };
  }

  /** True when any filter narrows the list (the view, group and sort are not filters). */
  function filtered(v) {
    return !!(v.q.trim() || v.bus.length || v.cat.length || v.tag.length || !v.fit);
  }

  /** What Reset clears: the filters and the order, not the person's choice of view. */
  function reset(v) {
    const d = defaults();
    return { ...d, view: v.view, group: v.group };
  }

  // -- what a driver is -----------------------------------------------------------------------------------------
  function busesOf(d) {
    if (d.versions && d.versions[0] && d.versions[0].bus) return d.versions[0].bus;
    return d.requires && d.requires.bus ? d.requires.bus : [];
  }

  function categoryOf(d) {
    return d.category || 'other';
  }

  function busLabel(kinds, id) {
    const k = kinds && kinds[id];
    return k ? k.label.split(' (')[0] : id;
  }

  /** The text a search runs over: name, chip, summary, tags, category, buses, package and capabilities. */
  function haystack(d, kinds) {
    return [d.name, d.chip, d.package, d.summary, categoryOf(d), ...(d.tags || []), ...busesOf(d),
      ...busesOf(d).map((b) => busLabel(kinds, b)),
      ...(d.capabilities || []).map((c) => c.label + ' ' + c.text)].filter(Boolean).join(' ').toLowerCase();
  }

  function matches(d, v, kinds, cache) {
    const tokens = v.q.toLowerCase().split(/\s+/).filter(Boolean);
    let text = cache && cache.get(d.name);
    if (text === undefined) {
      text = haystack(d, kinds);
      if (cache) cache.set(d.name, text);
    }
    if (!tokens.every((t) => text.includes(t))) return false;
    if (v.bus.length && !busesOf(d).some((b) => v.bus.includes(b))) return false;
    if (v.cat.length && !v.cat.includes(categoryOf(d))) return false;
    if (v.tag.length && !(d.tags || []).some((t) => v.tag.includes(t))) return false;
    return true;
  }

  /**
   * Filter the catalogue. `unavailable` is a Map of driver name to {reason}; with `fit` on (the default) those drivers
   * leave the result and are returned apart, still narrowed by the other filters, for the "Show N unavailable" list.
   * Within one filter group (bus, category, tag) any chip matches; between groups, all must.
   */
  function filter(drivers, v, unavailable, kinds, cache) {
    const out = { items: [], unavailable: [] };
    for (const d of drivers) {
      if (!matches(d, v, kinds, cache)) continue;
      if (unavailable && unavailable.has(d.name) && v.fit) out.unavailable.push(d);
      else out.items.push(d);
    }
    return out;
  }

  // -- sort and group -------------------------------------------------------------------------------------------
  const cmp = (a, b) => (a < b ? -1 : a > b ? 1 : 0);

  function sortKey(d, key, kinds) {
    if (key === 'category') return categoryOf(d);
    if (key === 'bus') return busesOf(d).map((b) => busLabel(kinds, b)).join(' ').toLowerCase();
    return d.name;
  }

  /** A sorted copy; ties fall back to the name, so the order is stable and the same everywhere. */
  function sort(list, key, desc, kinds) {
    const k = SORTS.includes(key) ? key : 'name';
    const out = list.slice().sort((a, b) => cmp(sortKey(a, k, kinds), sortKey(b, k, kinds)) || cmp(a.name, b.name));
    return desc ? out.reverse() : out;
  }

  /** Groups by category or by the first bus, in label order; the order inside a group is the order given. */
  function group(list, by, kinds) {
    const label = (d) => {
      if (by === 'bus') { const b = busesOf(d)[0]; return b ? busLabel(kinds, b) : 'No bus'; }
      return categoryOf(d);
    };
    const map = new Map();
    for (const d of list) {
      const l = label(d);
      if (!map.has(l)) map.set(l, []);
      map.get(l).push(d);
    }
    return [...map.keys()].sort(cmp).map((l) => ({ key: l, label: l, items: map.get(l) }));
  }

  // -- chunks ---------------------------------------------------------------------------------------------------
  /** How many items to render: all of a result up to PAGED_ABOVE, else `shown` (CHUNK, then more) of it. */
  function limit(total, shown) {
    if (total <= PAGED_ABOVE) return total;
    return Math.min(total, Math.max(CHUNK, shown | 0));
  }

  /** The first n items of groups, in order, dropping the groups that end up empty. */
  function take(groups, n) {
    const out = [];
    let left = n;
    for (const gr of groups) {
      if (left <= 0) break;
      const items = gr.items.slice(0, left);
      left -= items.length;
      out.push({ ...gr, items });
    }
    return out;
  }

  // -- the cascade ----------------------------------------------------------------------------------------------
  /** The selection availability() judges, from the page's image: family, target, RTOS, and the declared standard. */
  function selectionOf(image) {
    const sel = { family: image.mcu.family, target: image.mcu.target, rtos: image.rtos };
    if (image.cxx) sel.language = { standard: image.cxx };
    if (image.kb_core) sel.kb_core = image.kb_core;
    return sel;
  }

  /** Availability of every driver as a Map of name to {code, reason} for the unavailable ones; empty without an engine. */
  function unavailableMap(catalog, image) {
    const m = new Map();
    if (!KB.engine || typeof KB.engine.availability !== 'function') return m;
    for (const u of KB.engine.availability(catalog, selectionOf(image)).unavailable) m.set(u.name, u);
    return m;
  }

  function clone(image) {
    return JSON.parse(JSON.stringify(image));
  }

  /**
   * What a change took away. `before` and `after` are images; `unavailable` is the Map for `after`. Returns
   * {drivers: [{name, reason}], rtos: {from, to, why} | null, kept: [driver entries that still fit]}. The caller keeps
   * `kept`, so a driver that still fits stays, with its version and bus.
   */
  function cascade(before, after, unavailable, catalog) {
    const dropped = [];
    const kept = [];
    for (const d of after.drivers) {
      const u = unavailable.get(d.name);
      if (u) dropped.push({ name: d.name, reason: u.reason });
      else kept.push(d);
    }
    let rtos = null;
    if (before.rtos && before.rtos !== after.rtos) {
      const fam = (catalog.families || []).find((f) => f.id === after.mcu.family);
      const label = (id) => ((catalog.rtos || []).find((r) => r.id === id) || { label: id }).label;
      rtos = { from: before.rtos, to: after.rtos, toLabel: label(after.rtos).replace(/ \(.*$/, ''), why: `${label(before.rtos)} is not available on ${fam ? fam.label : after.mcu.family}` };
    }
    return { drivers: dropped, rtos, kept };
  }

  /** One line for the live region and the note: what dropped and why. Empty when nothing did. */
  function describe(drops) {
    const parts = [];
    if (drops.rtos) parts.push(`${drops.rtos.why}; now ${drops.rtos.toLabel}`);
    for (const d of drops.drivers) parts.push(`${d.name} removed: ${d.reason}`);
    return parts.join('. ');
  }

  /** The RTOS backends a family allows, in catalogue order. */
  function rtosFor(catalog, familyId) {
    const fam = (catalog.families || []).find((f) => f.id === familyId);
    return fam ? (catalog.rtos || []).filter((r) => fam.rtos.includes(r.id)) : [];
  }

  /** The C++ standard a family starts from (its default_language), else 20. */
  function defaultStandard(catalog, familyId) {
    const fam = (catalog.families || []).find((f) => f.id === familyId);
    return fam && fam.default_language && fam.default_language.standard ? fam.default_language.standard : 20;
  }

  /** Drivers on one bus of a shared kind (I2C, say), for the tray's hint: [{bus, names}] in bus order. */
  function sharedBuses(selected, kinds) {
    const by = new Map();
    for (const s of selected) {
      if (!s.bus || (kinds[s.via] && kinds[s.via].shared === false)) continue;
      if (!by.has(s.bus)) by.set(s.bus, []);
      by.get(s.bus).push(s.name);
    }
    return [...by.entries()].filter(([, names]) => names.length > 1).sort((a, b) => cmp(a[0], b[0]))
      .map(([bus, names]) => ({ bus, names }));
  }

  KB.picker = {
    VIEWS, GROUPS, SORTS, CHUNK, PAGED_ABOVE, defaults, sanitize, filtered, reset, busesOf, categoryOf, busLabel, haystack, filter,
    sort, group, limit, take, selectionOf, unavailableMap, clone, cascade, describe, rtosFor, defaultStandard, sharedBuses,
  };
})(typeof globalThis !== 'undefined' ? globalThis : this);
