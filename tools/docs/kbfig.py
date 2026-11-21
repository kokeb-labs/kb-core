# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
"""The kb-core figure kit: one vocabulary for every figure of the site, on top of figkit.

figkit (``kokeb_mkdocs_theme.figkit``) owns the brand colour tokens (light and dark), the type scale, stroke widths,
corner radii, arrowheads, the contrast and fit checks and the SVG output. This module adds what kb-core figures share
and figkit leaves to the figure:

* **Roles.** One colour per kind of thing, the same in every figure (``ROLE``). A reader learns the palette once.
* **Layout helpers.** ``need_h`` and ``row`` (equal cards in a row), ``lane`` (a labelled band of cards), ``sequence``
  (lifelines and messages), ``loop`` (a self-transition) and ``flow`` (a left to right pipeline of steps).
* **Arrow language.** Solid = "depends on" or "calls", dashed = "may use", "optional" or "returns". Green = allowed,
  rose = forbidden or a fault path. Colour never carries meaning alone: every rose or green edge has a text label.

Figure modules live in ``docs/diagrams/figures/<id>.py`` and are built by ``kokeb-docs figures`` (``--check`` in CI).
The build puts ``tools/docs`` on ``sys.path``, so a module says ``import kbfig``. docs/figures.md says how to add one.
"""

from __future__ import annotations

import math
from collections.abc import Iterable, Sequence

from kokeb_mkdocs_theme.figkit import Box, Fig, text_width, wrap

#: What a thing is -> the figkit tone that draws it. Keep this list short; add a role only for a new kind of thing.
ROLE: dict[str, str] = {
    "core": "brand",  # kb::utils and kb-core as a whole: the Kokeb copper (gold in the dark theme)
    "contract": "blue",  # kb::interfaces concepts, port contracts
    "handler": "violet",  # kb::handlers, bindings, the start-up pipeline
    "rtos": "amber",  # kb::rtos and its backends, timing
    "port": "teal",  # MCU ports, the host port
    "driver": "green",  # driver-<chip>
    "product": "magenta",  # the application and the board HAL: code the product team owns
    "soup": "slate",  # SOUP, planned or out-of-scope things (draw dashed when planned)
    "ok": "green",  # allowed, passes
    "bad": "rose",  # forbidden, fails, fault path
    "tool": "brown",  # generators, the configurator, the kb tool
}


def tone(role: str) -> str:
    """The figkit tone of a role; a tone name passes through unchanged."""
    return ROLE.get(role, role)


def legend(*roles: str, extra: Sequence[tuple[str, str]] = ()) -> list[tuple[str, str]]:
    """A figure legend from role names: ``legend("core", "contract")``; ``extra`` adds raw ``(tone, label)`` pairs."""
    names = {
        "core": "kb::utils",
        "contract": "interfaces and contracts",
        "handler": "handlers",
        "rtos": "kb::rtos",
        "port": "MCU ports",
        "driver": "drivers",
        "product": "product and board HAL",
        "soup": "SOUP and planned",
        "ok": "allowed",
        "bad": "forbidden or fault",
        "tool": "tools and generators",
    }
    return [(tone(r), names[r]) for r in roles] + list(extra)


def need_h(f: Fig, w: float, title: str, lines: Iterable[str] = (), sub: str = "", mono: bool = False) -> float:
    """Height ``Fig.card`` gives a card of width ``w`` with this content (so a row of cards can share one height)."""
    k = f.k
    pad = k["pad"]
    inner_w = w - 2 * pad - 6
    size = k["mono"] if mono else k["card_body"]
    t_lines = wrap(title, inner_w, k["card_title"], 700)
    s_lines = wrap(sub, inner_w, k["small"]) if sub else []
    body: list[str] = []
    for ln in lines:
        body += wrap(ln, inner_w, size, 400, mono)
    need = pad + len(t_lines) * k["card_title"] * 1.3
    need += (len(s_lines) * k["small"] * 1.35 + 2) if s_lines else 0
    need += (6 + len(body) * size * 1.45) if body else 0
    return math.ceil(need + pad - 4)


def row(
    f: Fig,
    x: float,
    y: float,
    w: float,
    cards: Sequence[dict],
    gap: float = 20,
    weights: Sequence[float] | None = None,
) -> list[Box]:
    """Cards side by side in a band ``w`` wide, all as tall as the tallest.

    Each card is a dict: ``title`` (required), ``lines``, ``sub``, ``role`` (or ``tone``), ``tag``, ``accent``,
    ``dashed``, ``mono``, ``center``. ``weights`` sets relative widths (default equal).
    """
    n = len(cards)
    ws = list(weights) if weights else [1.0] * n
    avail = w - gap * (n - 1)
    widths = [avail * v / sum(ws) for v in ws]
    h = max(need_h(f, wi, c["title"], c.get("lines", ()), c.get("sub", ""), c.get("mono", False))
            for wi, c in zip(widths, cards))
    out, cx = [], x
    for wi, c in zip(widths, cards):
        out.append(f.card(cx, y, wi, c["title"], c.get("lines", ()), tone=tone(c.get("role", c.get("tone", "slate"))),
                          h=h, sub=c.get("sub", ""), mono=c.get("mono", False), tag=c.get("tag", ""),
                          accent=c.get("accent", False), dashed=c.get("dashed", False),
                          center=c.get("center", False)))
        cx += wi + gap
    return out


def lane(f: Fig, x: float, y: float, w: float, label: str, cards: Sequence[dict], role: str = "soup", sub: str = "",
         gap: float = 18, weights: Sequence[float] | None = None, dashed: bool = False) -> tuple[Box, list[Box]]:
    """A tinted band with a label and a row of cards inside it; returns the band and the cards."""
    inner = f.region_inner_top(y, sub_below=bool(sub))
    pad = 16
    n = len(cards)
    ws = list(weights) if weights else [1.0] * n
    avail = w - 2 * pad - gap * (n - 1)
    widths = [avail * v / sum(ws) for v in ws]
    h = max(need_h(f, wi, c["title"], c.get("lines", ()), c.get("sub", ""), c.get("mono", False))
            for wi, c in zip(widths, cards))
    band = f.region(x, y, w, inner - y + h + pad, label, tone=tone(role), sub=sub, sub_below=bool(sub), dashed=dashed)
    boxes = row(f, x + pad, inner, w - 2 * pad, cards, gap=gap, weights=weights)
    return band, boxes


def flow(f: Fig, x: float, y: float, w: float, steps: Sequence[dict], role: str = "handler", gap: float = 34,
         label_arrows: Sequence[str] | None = None) -> list[Box]:
    """A left to right pipeline: numbered cards joined by arrows. ``steps`` are ``row`` dicts."""
    boxes = row(f, x, y, w, steps, gap=gap)
    for i, (a, b) in enumerate(zip(boxes, boxes[1:])):
        lab = label_arrows[i] if label_arrows and i < len(label_arrows) else ""
        f.edge([(a.r + 4, a.cy), (b.x - 4, b.cy)], tone="rail", label=lab)
    return boxes


def loop(f: Fig, b: Box, side: str = "right", label: str = "", tone_: str = "rail", r: float = 22) -> None:
    """A self-transition: a small rounded loop on a side of ``b`` with an optional label beside it."""
    if side == "right":
        a, c = (b.r + 4, b.cy - 10), (b.r + 4, b.cy + 10)
        pts = [a, (b.r + r, b.cy - 10), (b.r + r, b.cy + 10), c]
        lx, ly = b.r + r + 8 + text_width(label, f.k["edge_label"]) / 2, b.cy
    elif side == "top":
        a, c = (b.cx - 12, b.y - 4), (b.cx + 12, b.y - 4)
        pts = [a, (b.cx - 12, b.y - r), (b.cx + 12, b.y - r), c]
        lx, ly = b.cx, b.y - r - 14
    else:
        raise ValueError(side)
    f.edge(pts, tone=tone_)
    if label:
        f.edge_label(lx, ly, label)


def sequence(
    f: Fig,
    y: float,
    actors: Sequence[tuple[str, str, str]],
    messages: Sequence[tuple],
    x: float = 0.0,
    w: float | None = None,
    step: float = 54,
) -> float:
    """A sequence diagram: ``actors`` are ``(id, label, role)``; ``messages`` are tuples
    ``(from, to, label)`` (a call, solid), ``(from, to, label, "ret")`` (a return, dashed), ``(id, id, label, "self")``
    (a call to itself) or ``("note", id, text)`` (a call-out under a lifeline). Returns the y under the last message.
    The arrow tone is the role of the sender.
    """
    w = f.body_w - x if w is None else w
    n = len(actors)
    col = w / n
    pos = {a[0]: x + col * (i + 0.5) for i, a in enumerate(actors)}
    role_of = {a[0]: a[2] for a in actors}
    head_h = max(need_h(f, col - 14, label) for _i, label, _r in actors)
    heads = {}
    for aid, label, role in actors:
        heads[aid] = f.card(pos[aid] - (col - 14) / 2, y, col - 14, label, tone=tone(role), h=head_h, accent=True,
                            center=True)
    cy = y + head_h + 38
    total = 0.0
    for m in messages:
        if m[0] == "note":
            total += max(step, need_h(f, col - 12, m[2]) + 14)
        else:
            total += step
    bottom = cy + total - 24
    for aid, _label, role in actors:
        f.line([(pos[aid], y + head_h + 2), (pos[aid], bottom)], color="line", sw=1.4, dash="5 5", layer="bg")
    n_step = 0
    for m in messages:
        if m[0] == "note":
            _n, aid, text = m
            nb = f.note(pos[aid] - col / 2 + 6, cy - 14, col - 12, text, tone=tone(role_of[aid]))
            cy += max(step, nb.h + 14)
            continue
        src, dst, label = m[0], m[1], m[2]
        kind = m[3] if len(m) > 3 else "call"
        n_step += 1
        t = tone(role_of[src])
        if kind == "self":
            sx = pos[src]
            f.edge([(sx + 3, cy - 8), (sx + 38, cy - 8), (sx + 38, cy + 12), (sx + 3, cy + 12)], tone=t)
            f.edge_label(sx + 46 + text_width(label, f.k["edge_label"], 500) / 2, cy + 2, label)
        else:
            sx, dx = pos[src], pos[dst]
            d = 1 if dx > sx else -1
            f.edge([(sx + 4 * d, cy), (dx - 4 * d, cy)], tone=t, dashed=(kind == "ret"))
            lw = text_width(label, f.k["edge_label"], 500) + 12
            if lw > abs(dx - sx) - 8:
                f.problem(f"message {label!r} is wider than the space between its lifelines")
            f.edge_label((sx + dx) / 2, cy - 15, label)
        cy += step
    return bottom + 10


def pairs(f: Fig, x: float, y: float, w: float, rows: Sequence[tuple], edge_tone: str = "ok", dashed: bool = False,
          row_h: float = 58, arrow_at: float = 0.4) -> float:
    """Rows of ``(from, from_role, to, to_role, note)``: two pills joined by an arrow, the note under them.

    The arrow goes from a fixed column so the target pills line up. Returns the y under the last row.
    """
    ax0 = x + w * arrow_at
    for a, ar, b, br, note in rows:
        pa = f.pill(x, y, a, tone=tone(ar), on="canvas")
        pb_x = ax0 + 54
        f.edge([(max(pa.r, ax0 - 40) + 8, pa.cy), (pb_x - 8, pa.cy)], tone=edge_tone, dashed=dashed)
        f.pill(pb_x, y, b, tone=tone(br), on="canvas")
        if note:
            f.text(x + 4, y + pa.h + f.k["small"] + 6, note, size=f.k["small"], color="muted")
        y += row_h if note else row_h - 20
    return y


def title_case_alt(*parts: str) -> str:
    """Join sentences for an ``alt`` text (one line, single spaces)."""
    return " ".join(" ".join(p.split()) for p in parts)
