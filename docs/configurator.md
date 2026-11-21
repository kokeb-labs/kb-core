# Using the configurator

The configurator is a page that turns a few choices into a pinned, repeatable kb-core project with its evidence
([ADR-0013](adr/ADR-0013-hosted-configurator.md), [ADR-0014](adr/ADR-0014-kb-core-is-the-assembler.md)). It runs
entirely in the browser: nothing is sent anywhere, no cookie is set, and the selection lives in the address bar,
so a copied link reproduces it exactly. To use it without a network, take the offline copy (the link in its footer,
or `configurator/offline.html` in the repository) and open it from disk.

**[Open the configurator](app/index.html)**: pick a platform, add drivers, download the project. The defaults
already resolve, so a first project takes four clicks.

## The main path

Three steps, in this order; each one shows only what the earlier ones allow.

1. **Target.** Pick the MCU family (ESP32, STM32, host), the target, the RTOS and the C++ standard. Only the RTOS
   backends the family lists are offered (`rtos` and `default_rtos` of the family in the catalogue), and the standard
   starts at the family's `default_language`. A family marked *planned* has no port yet and cannot be built. Pins and
   board mapping are not chosen here: they come with your board layer, so the page uses each target's default board
   internally and the generated files do not depend on it.
2. **Drivers.** Cards with the chip pinout, the name, a one-line purpose, the buses and the category, each with
   *Add* or *Remove*. The *Selected* tray lists what you chose and says when two drivers share a bus. Search runs over
   name, summary, tags, category and bus; chips filter by bus, category and tag (each with its count from `tag_index`)
   and by *Works with my target*; *Reset* clears the filters and the order, and a line says how many drivers match.
3. **Output.** *Download starter project (.zip)* and *Copy share link*. The defaults already resolve, so a first
   project is the target, two drivers and the download: four clicks, which `headless_check.mjs` counts and fails above.

The result bar (small screens) and the *Check* panel keep the findings of the shared rules and the site checks in view.

## Cascading selection

A choice shapes the next ones. After every change of family, target, RTOS or C++ standard the page asks
`KB.engine.availability()` (the shared resolver, `resolver.mjs`; see Tags and availability below) which drivers the new
selection can use. The page holds no rule of its own, so a new port or driver needs no page change.

- Unavailable drivers are hidden. *Show N unavailable* lists them with the one-line reason (the MCU family, the
  peripheral or bus the target lacks, the RTOS, the kb-core or the language floor). Turning *Works with my target* off
  lists every driver in the main list, the unavailable ones with their reason and no *Add*.
- A change keeps whatever still fits. If it drops a chosen driver or the RTOS, one line says what and why
  ("ws2812_rmt removed: needs MCU family esp32, not host") with an *Undo* button, and a live region announces it.
  *Undo* restores the earlier selection whole.
- A link that already holds a driver the target cannot use is kept as it is: the tray marks it *unavailable* with the
  reason and the Check panel reports the resolver's error. Only a change you make drops anything.

## Views

The driver list has three views, remembered per viewer in `localStorage` (the key `kb-configurator-view`; the page
works without it): **Cards** (the default), a compact **List** (a table whose Driver, Bus and Category headers sort)
and **Grouped** (cards under headings, grouped by category or by bus). *Sort by* (name, category, bus) with its
direction and the filters work with every view and with the grouping. A result of up to 60 drivers is shown whole; a
longer one shows 24 and a *Show more* button.

The filters, the sort, the grouping and the view are in the link, so it reproduces what you saw. They are optional
parameters of the same hash, written only when they differ from the defaults and read only when `w=1` is present, so
every older link keeps its meaning:

| Parameter | Meaning |
|---|---|
| `w=1` | The view parameters follow (version 1) |
| `q=` | Search text, lower case, letters, digits, space and `. _ -`, at most 40 characters |
| `fb=`, `fc=`, `ft=` | Comma lists of bus, category and tag chips |
| `fw=0` | *Works with my target* is off |
| `vw=l` or `vw=g` | List or grouped view (cards when absent) |
| `gb=b` | Group by bus (by category when absent) |
| `so=c` or `so=b` | Sort by category or bus (by name when absent); a leading `-` reverses |
| `su=1` | The unavailable drivers are listed |

## Advanced

One closed disclosure, *Advanced*, holds everything a first project does not need:

- **Open an existing project**, with the upgrade report and the downloads (see below), and *Offer release candidates*;
- **Versions and name**: the kb-core and port versions (the newest by default), the image name, and *Show release
  candidates*, which also lets a driver with only release candidates be added;
- **Drivers, buses and thread models**: each chosen driver's version, bus kind and bus, and one thread model per bus
  ([ADR-0009](adr/ADR-0009-handler-thread-models.md)): an **owner task** that serves every device on it, or a
  **guarded facade** that locks around each call. A driver that must be serviced continuously (bno08x) makes its bus
  an owner bus when you add it;
- **Inside and tested**: the *Architecture* map, the *Evidence* of the kb-core release (also on
  [Test evidence](assurance/evidence.md)) and *Versions*, read from the release's CI artifacts;
- **Preview files**: every generated file, with *Copy* and *Download*. The zip stays the main output.

## Open an existing project

*Open an existing project*, in *Advanced*, takes your project's `kb-image.yml` and its `kb-image.lock.yml` (pick
both at once; a lock alone also works). The page reads them in the browser with the file picker: nothing is
uploaded or stored. The address bar gets only catalogue identifiers: the project's own image name stays out of it
while the project is open, and drivers this catalogue does not list are left out of the selection (and named under
the file picker). The project becomes the selection, with every version taken from the lock,
and **Available upgrades** lists one row per component (kb-core, an extracted port, each driver):

- the current version and the version it can move to: the newest **final release** that passes the gates against
  the new kb-core (MCU family, `kb_core_min`, the owner-task rule, the bus kind in use); tick *Offer release
  candidates* (the same switch as in *Versions and name*) to include rcs, or pick a version per row (the same as
  `kb upgrade --to`); versions that are held back are listed but cannot be picked, and **Clear chosen versions**
  undoes the picks;
- the change (`major`, `minor`, `patch`, `prerelease`, or `commit` when the lock pins another commit than the
  catalogue for the same version), marked *breaking* by SemVer (a new major; before 1.0 a new minor; before 0.1 a
  new patch), with a link to the new tag's release notes;
- the newest release and a newer release candidate, the versions *held back* and why;
- known vulnerabilities of both versions (or *not scanned*) and the ones the upgrade fixes.

**Use the upgraded versions** moves the selection to the plan; the Check panel then shows the usual findings
for it, including the tested-set status. **Download kb-upgrade.md**, **kb-image.yml** and **kb-image.lock.yml** give
the same bytes as `kb upgrade kb-image.yml --catalog data/catalog.json -o .` run on the page's own catalogue
(`configurator/data/catalog.json`, which `kb` reads as it is; [the kb tool](kb-tool.md#find-and-apply-upgrades)).
Both run the [upgrade rules](design/assembler.md#upgrades) of the shared specification, pass the same vectors, and
a test plans every driver of the shipped catalogue in both and compares the files. With a different catalogue (a
kb release's `catalog.json` built later, with other versions or scans) the answers differ as the catalogues do.

## Checks

Every change runs two rule sets and lists their findings in plain words, each with its code:

- the **shared rules** of the `kb` tool (`KB-RES-*`, [Assembler and resolver](design/assembler.md)): MCU gates,
  RTOS per family, `kb_core_min`, one thread model per bus, `kRequiresOwner`, versions that exist;
- the **site checks** (`CFG-*`, [Configurator checks](configurator-checks.md)): bus instances of the target, a
  point-to-point UART shared by two drivers, Modbus and plain serial on one line, I2C address clashes, the pin
  budget, handler bindings, tested version sets and known vulnerabilities.

Errors block the downloads; warnings (a release candidate, an untested combination, a development revision) do
not.

## Downloads

| File | What it is |
| --- | --- |
| `kb-image.yml` | The selection (schema 1); `kb resolve kb-image.yml` gives the same lock |
| `kb-image.lock.yml` | Every pin by tag and commit, with the MCU, RTOS, parts, buses and CMake values |
| `CMakePresets.json`, `kb_image.cmake` | `KB_MCU`, `KB_RTOS`, `KB_PARTS`, `KB_ENABLE_*` and each `KB_DRIVER_<NAME>_DIR` (`external/drivers/`) for kb-core's build |
| `kb-fetch.sh` | Adds kb-core, the port and every driver to your repository as git submodules under `external/` at their tags, checks every commit, refuses a moved tag, records no branch, never recurses; `--verify` re-checks them in CI ([layout](design/assembler.md#layout)) |
| `evidence/sbom.cdx.json`, `evidence/sbom.spdx.json` | CycloneDX 1.6 and SPDX 2.3 SBOMs of exactly the selection, identifiers derived from the lock, plus the SDK and RTOS as SOUP |
| `evidence/licences.md` | Each component's SPDX licence and copyright holder |
| `evidence/status.md` | The tested version set or an *untested combination* warning, the known-vulnerability scan of each component (or *not scanned*), the compatibility rows and the findings |
| Starter project (`.zip`) | All of the above plus an ESP-IDF or CMake project skeleton with a board layer |

The lock, the presets, `kb_image.cmake` and `kb-fetch.sh` are written by the same resolver code as `kb resolve`, so
they match byte for byte. Every file is repeatable: the same selection gives the same bytes, in any time zone; the
timestamp is the newest commit date of the selected components.

## Where the facts come from

The catalogue (`configurator/data/catalog.json`) is generated by `configurator/tools/build_catalog.py` from the
driver repositories: for every `vX.Y.Z` and `vX.Y.Z-rc.N` tag it reads `driver.yml`, the README's summary and
*At a glance* table, `LICENSE`, `docs/hardware.yml` and the chip figures at that tag's commit, never from a working
tree. A tag whose commit differs between a local clone and GitHub stops the build. Known-vulnerability results
come from the scan in CI (osv-scanner); a component without a scan shows *not scanned*. Bus counts and free pins
of targets and boards are approximate where the catalogue says so; check the board's schematic.

## Tags and availability

Each driver carries search tags and one category, so the page only filters. They come from the GitHub topics of the
driver repository, read by `build_catalog.py --online` with `gh api repos/<org>/<repo>/topics` and mapped by the rules
in `configurator/data/tags.json` (schema `configurator/schemas-draft/tags.schema.json`):

| Rule | Meaning |
|---|---|
| `drop` | Topics every driver has (`cpp20`, `driver`, `embedded`, `header-only`, `kokeb-labs`, `misra-cpp`, `kb-driver`) and the bus names (`spi`, `i2c`, `uart`, `can`, `modbus`); never tags. `adc` is not a bus here and stays a tag |
| `alias` | A variant spelling to the allowed tag it means (`motor-drive` and `gate-driver` to `motor-driver`, `ntc` to `temperature-sensor`) |
| `allow` | The tags the page offers as chips; a topic that is none of the above (chip and vendor names, anything unknown) is not a tag and stays searchable through the card's own text |
| `category_of` | Each allowed tag to one category: sensor, motor, memory, io, radio, secure-element or other. A driver's category is the one most of its tags have; a tie goes to the category listed first in `categories` |
| `fallback` | A driver left with no allowed tag takes `category` from `driver.yml`, else tags from keywords of its description or README summary, else a category from its bus, else `other`; the category is never empty |

The catalogue stores `tags` (sorted) and `category` per driver and a `tag_index` with the number of drivers per tag and
per category, for the chip counts. The raw topics are not stored. Without `--online` the topics are not fetched and
nothing is reused from an earlier build: the builder says so, tags come from the fallback only, and `--check` skips
them; CI runs `--check --online`, which compares them. Commit a catalogue only after an online build.

To add a tag: add it to `allow` and `category_of` in `tags.json` (and an `alias` for each spelling that should map to
it), put the topic on the driver repositories, rebuild with `--online`, and commit the catalogue. `load_rules` refuses
an alias that names a tag not in `allow`, a tag without a category and a tag that is also dropped.

Availability is data too. Each driver has `requires` (bus kinds, MCU families, peripheral, RTOS backends, kb-core
minimum and language floor of its newest version), each family has `default_language`, `rtos` and `default_rtos`, and
each target has `peripherals`. `availability(catalog, selection)` in `configurator/resolver.mjs` and
`tools/kb/resolver.py` returns `{available, unavailable: [{name, code, reason}]}` for a family, target, RTOS and
language standard, with no rule in the page; both give the same answer (shared vectors 033 and up). A driver that fails
several
gates reports the first of: MCU family (`KB-RES-013`), peripheral (`KB-RES-031`), bus (`KB-RES-032`), RTOS
(`KB-RES-033`), kb-core (`KB-RES-014`), language (`KB-RES-024`, `KB-RES-025`); a selection that cannot build at all
(`KB-RES-003` to `KB-RES-008`, for example no port yet) blocks every driver with that one reason.

Contact: <kokeblabs@gmail.com>
