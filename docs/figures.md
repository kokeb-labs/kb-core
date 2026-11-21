# Figures

Every diagram of this site is a committed SVG pair, light and dark, drawn from a small Python module by the
documentation toolchain (`figkit`, shipped in the shared `kokeb-mkdocs-theme`) and the kb-core kit on top of it
(`tools/docs/kbfig.py`). There is no script in the reader's browser: the pages load no Mermaid, stay fast on a phone and
print. The [figure catalogue](diagrams/index.md) lists every figure with its description.

## What every figure has

- **One source.** `docs/diagrams/figures/<id>.py` is the only place a figure is edited. A figure that shows facts of
  the repository (the components, a rule file) reads them in `data()`, so it cannot drift from the code.
- **A light and a dark file.** `<id>.svg` and `<id>.dark.svg`, laid out once, so the two cannot differ. Pages embed
  both with a marker pair and the site follows the theme toggle.
- **Accessible text.** One sentence of `alt` becomes the `<title>` and the `alt` of every embed; the `Reads:`
  paragraph of the module docstring becomes the `<desc>` and the long description under the figure. Colour is never the
  only carrier of meaning: edges that are forbidden or faulty are dashed or labelled, states carry a text.
- **Contrast of at least 4.5:1** for every text, in both themes, checked as the figure is built, and text sizes that
  stay readable at README width and on a phone.
- **A freshness check.** `kokeb-docs figures --check` fails CI when a committed SVG differs from a fresh build, and
  `tools/docs/check_figures.py` rejects Mermaid fences, `.mmd` files, a missing light or dark file or source, and an
  SVG with a script or an external reference.

## The palette

Colours are tokens, never hex values: the Kokeb copper (gold in dark) for kb-core itself, and one tone for each kind of
thing, the same in every figure, so a reader learns it once.

| Role | Tone | Used for |
| --- | --- | --- |
| `core` | copper (gold in dark) | kb-core as a whole, `kb::utils` |
| `contract` | blue | `kb::interfaces`, port contracts, shared state, inputs |
| `handler` | violet | `kb::handlers`, bindings, the start-up pipeline |
| `rtos` | amber | `kb::rtos` and its backends, timing |
| `port` | teal | MCU ports, the host port, bus domains |
| `driver` | green | `driver-<chip>` (and "ok", "allowed", outputs) |
| `product` | magenta | the application and the board HAL |
| `soup` | slate, dashed when planned | SOUP and things not built yet |
| `bad` | rose | forbidden edges, failures, fault paths |
| `tool` | brown | generators, the kb tool, tasks and tests |

Solid arrows are calls or "depends on"; dashed arrows are optional, returns or forbidden (always with a label that says
which). Cards have a coloured stripe and 10 px corners; regions are tinted bands with an upper-case label.

## Templates

`kbfig` adds what figkit leaves to each figure:

| Call | Draws |
| --- | --- |
| `kbfig.row(f, x, y, w, cards)` | Cards side by side, equal height, with a role colour each |
| `kbfig.lane(f, x, y, w, label, cards, role=...)` | A tinted band with a label and a row of cards (a layer, a zone) |
| `kbfig.flow(f, x, y, w, steps)` | A left to right pipeline of cards joined by arrows |
| `kbfig.pairs(f, x, y, w, rows)` | Rows of two pills and an arrow with a note (allowed and forbidden edges) |
| `kbfig.sequence(f, y, actors, messages)` | A sequence diagram: lifelines, calls, returns, self calls, notes |
| `kbfig.loop(f, box, side, label)` | A self-transition on a state |
| `kbfig.legend(*roles)` | A legend from role names |

State machines are drawn with `f.card` per state on a grid and `f.edge` for each transition (see
`docs/diagrams/figures/handler-health.py`); a layered stack with `kbfig.lane` per layer (`layer-stack.py`).

## Add a figure

1. Copy the closest module in `docs/diagrams/figures/` to `<id>.py` (lower case, hyphens) and change its content. Keep
   a `Reads:` paragraph in the docstring, a one-sentence `alt` and a `legend` that names every colour you use.
2. Build and look at it: `kokeb-docs figures <id>` (from the repository root, with
   `KOKEB_DOCS_CONFIG=_config/mkdocs.yml`) writes `docs/diagrams/<id>.svg` and `<id>.dark.svg`. The build refuses a
   figure with low contrast, text that does not fit, overlapping cards, an arrow under a card or a label on a card;
   fix the layout rather than the check. `rsvg-convert -w 1200 docs/diagrams/<id>.svg -o /tmp/<id>.png` gives a
   picture to open.
3. Embed it in a page with a marker pair, and run `kokeb-docs figures` again to fill it:

   ```text
   <!-- figure: <id> -->
   <!-- /figure -->
   ```

4. Run `kokeb-docs check`, `python3 tools/docs/check_figures.py` and the strict site build, or `tools/preflight.sh`,
   which runs all three. Commit the module, the two SVGs, `figures.json` and `diagrams/index.md` together.

Rules of thumb: at most about ten cards per figure (split a bigger story in two), arrows only between neighbours or
along a clear gap, labels of a few words (put detail in the card text), the same role colour for the same thing as
everywhere else, and a figure that shows repository facts reads them in `data()` and names its `sources`.
