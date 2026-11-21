# Figures

Every diagram of this site is a committed SVG pair, light and dark, drawn from a small model file by the documentation
toolchain (`figkit`, shipped in the shared `kokeb-mkdocs-theme`). There is no script in the reader's browser: the pages
load no Mermaid, stay fast on a phone and print. The [figure catalogue](diagrams/index.md) lists every figure with its
description.

## What every figure has

- **One source.** `docs/model/<id>.<kind>.yml` is the only place a figure is edited. The kind is one of the shared
  figkit kinds: `state`, `graph`, `seq`, `timeline`, `matrix`, `bytes` or `pipeline`. A figure that shows facts of the
  repository (the components) is a small module `docs/diagrams/figures/<id>.py` instead: its `data()` reads the facts
  and its `build()` turns them into the same model and draws it with `from_model`, so it cannot drift from the code.
- **A phone-first layout.** About 640 px wide, read top to bottom, with short labels on the cards and arrows. Long
  text goes to the `reads` paragraph under the figure, not onto the figure.
- **A light and a dark file.** `<id>.svg` and `<id>.dark.svg`, laid out once, so the two cannot differ, plus
  `<id>.adaptive.svg`, one file for both themes. Pages embed the pair with a marker pair and the site follows the theme
  toggle.
- **Accessible text.** The one-sentence `alt` becomes the `<title>` and the `alt` of every embed; the `reads`
  paragraph becomes the `<desc>` and the long description under the figure. Colour is never the only carrier of
  meaning: forbidden or faulty edges are dashed and labelled, states and matrix cells carry a word, and a matrix also
  gets a table twin, `<id>.table.html`.
- **Checks.** Text contrast of at least 4.5:1 and shape contrast of at least 3:1 in both themes, text that fits its
  card, no arrow under a card, text of at least 7 pt when printed on A4, and SVG and alt lint, all from
  `kokeb-docs figures --lint`. `kokeb-docs figures --check` fails CI when a committed SVG differs from a fresh build,
  and `tools/docs/check_figures.py` rejects Mermaid fences, `.mmd` files, a missing light or dark file or source, a
  module that does not draw with figkit, and an SVG with a script or an external reference.

## The palette

Colours are tokens, never hex values: the Kokeb copper (gold in dark) for kb-core itself, and one tone for each kind of
thing, the same in every figure, so a reader learns it once. Write the tone in the model (`tone: violet`).

| Role | Tone | Used for |
| --- | --- | --- |
| core | `brand`, copper (gold in dark) | kb-core as a whole, `kb::utils` |
| contract | `blue` | `kb::interfaces`, port contracts, shared state, inputs |
| handler | `violet` | `kb::handlers`, bindings, the start-up pipeline |
| rtos | `amber` | `kb::rtos` and its backends, timing |
| port | `teal` | MCU ports, the host port, bus domains |
| driver | `green` | `driver-<chip>` (and "ok", "allowed", outputs) |
| product | `magenta` | the application and the board HAL |
| soup | `slate` | SOUP and things not built yet (a card without the accent tint) |
| bad | `rose` | forbidden edges, failures, fault paths |
| tool | `brown` | generators, the kb tool, tasks and tests |

A matrix uses the status tones instead: `ok`, `warn`, `fail`, `na` and `planned`, each drawn with a mark and a word.
Solid arrows are calls or "depends on"; dashed arrows are optional, returns or forbidden, always with a label that says
which. Every figure has a `legend` that names each tone it uses.

## Choosing a kind

| Kind | For | Examples here |
| --- | --- | --- |
| `graph` | Layers, flows and call chains: cards and arrows in ranks, top to bottom | `layer-stack`, `ci-flow`, `call-sequence` |
| `state` | A state machine: states, transitions, a start disc, self loops | `handler-health`, `dma-lease` |
| `seq` | Messages between three or four participants over time | `async-poll` |
| `matrix` | A verdict per row, with a word and a mark in every cell | `dependency-rules`, `misra-scope` |
| `pipeline` | Columns of stacked items | `handler-startup` |
| `timeline` | Lanes against a time axis | none yet |
| `bytes` | A record laid out byte by byte | none yet |

Keep a `graph` or `state` figure to about two cards per rank: a rank of three cards makes the figure wider than
640 px. A `seq` figure with five participants is too wide for a phone; draw the call chain as a `graph` with solid calls
going down and dashed returns going up instead (see `call-sequence`). One figure, `handler-threading`, stays a module of
plain figkit calls, because it stacks two small graphs one above the other, which the `graph` kind would put side by
side.

## Add a figure

1. Scaffold it: `kokeb-docs figkit new --kind graph --id <id> --page docs/<page>.md` (from the repository root, with
   `KOKEB_DOCS_CONFIG=_config/mkdocs.yml`). This writes `docs/model/<id>.graph.yml` and adds the embed marker to the
   page.
2. Edit the model. Write the one-sentence `alt` (40 to 300 characters) and the `reads` paragraph first: at least two
   sentences, naming every state, participant, lane or column. Then the cards, the edges and the `legend`. Quote a list
   item that holds a comma or a colon (`lines: ["MCU, RTOS"]`), or YAML splits it.
3. For a `state` or `graph` figure, run `kokeb-docs figkit layout <id>` and commit `docs/diagrams/<id>.layout.json`.
   This is the only step that needs Graphviz; run it again whenever the cards, edges or text change.
4. Build and look at it: `kokeb-docs figures` writes the SVGs, `figures.json` and the catalogue, and fills the embed
   markers. The build refuses low contrast, text that does not fit, overlapping cards, an arrow under a card or a label
   on a card; fix the model rather than the check. `rsvg-convert -w 360 docs/diagrams/<id>.svg -o /tmp/<id>.png` shows
   it at phone width.
5. Run `kokeb-docs figures --lint`, `kokeb-docs check`, `python3 tools/docs/check_figures.py` and the strict site
   build, or `tools/preflight.sh`, which runs them. Commit the model, the layout lock and everything under
   `docs/diagrams/` together.

Rules of thumb: at most about ten cards per figure (split a bigger story in two), labels of a few words, the same tone
for the same thing as everywhere else, and a figure that shows repository facts reads them in `data()` and names its
`sources`. Never edit an SVG or a layout lock by hand, write a hex colour, or put a time stamp in a model.
