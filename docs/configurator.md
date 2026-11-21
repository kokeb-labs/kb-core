# Using the configurator

The configurator is a page that turns a few choices into a pinned, repeatable kb-core project with its evidence
([ADR-0013](adr/ADR-0013-hosted-configurator.md), [ADR-0014](adr/ADR-0014-kb-core-is-the-assembler.md)). It runs
entirely in the browser: nothing is sent anywhere, no cookie is set, and the selection lives in the address bar,
so a copied link reproduces it exactly. To use it without a network, open `configurator/offline.html` from disk.

## The four steps

1. **Platform.** Pick the MCU family (ESP32, STM32, host), the target and the board. The target fixes how many
   I2C, SPI, UART, CAN and RMT instances exist; a named development board also gives the number of free pins.
   A family marked *planned* has no port yet and cannot be built.
2. **RTOS and versions.** Pick the RTOS backend the family allows (ESP-IDF runs FreeRTOS only; ThreadX on the
   families that list it), the kb-core version and, once the port has its own repository, the port version.
   Until kb-core has a release, the catalogue offers its development revision, pinned by commit and flagged with a
   warning.
3. **Drivers.** Search the catalogue by chip, bus or capability, or filter by bus. Each card shows the chip, its
   package, the buses it speaks, its capabilities, its licence, its status and a pinout or block diagram (click it
   for every figure). Each driver has a version selector with its published releases; the default is kb-core's pin,
   else the newest release. Release candidates (marked *rc*) are **hidden by default**: turn on *Show release
   candidates* to list them and to add a driver that has only release candidates. The switch is kept in the link
   (`c=1`) and turns on by itself when a link or an opened project already uses an rc. A search with no match offers
   **request it**: a link to the driver request form of `kokeb-labs-drivers/driver-requests`, prefilled with the
   search text reduced to letters, digits, `.`, `_` and `-`; the footer links to the request forms for a driver and
   for a port or target. The page itself sends nothing.
4. **Buses and thread models.** Each driver goes on a bus (`i2c0`, `spi1`, `uart1`, `pio0`, ...). Give each bus one
   thread model ([ADR-0009](adr/ADR-0009-handler-thread-models.md)): an **owner task** that serves every device
   on it, or a **guarded facade** that locks around each call. Chips that must be serviced continuously (bno08x)
   need an owner task. The pin meter adds up the bus pins and the optional control pins.

## Open an existing project

*Open an existing project*, above step 1, takes your project's `kb-image.yml` and its `kb-image.lock.yml` (pick
both at once; a lock alone also works). The page reads them in the browser with the file picker: nothing is
uploaded or stored. The address bar gets only catalogue identifiers: the project's own image name stays out of it
while the project is open, and drivers this catalogue does not list are left out of the selection (and named under
the file picker). The project becomes the selection of the four steps, with every version taken from the lock,
and **Available upgrades** lists one row per component (kb-core, an extracted port, each driver):

- the current version and the version it can move to: the newest **final release** that passes the gates against
  the new kb-core (MCU family, `kb_core_min`, the owner-task rule, the bus kind in use); tick *Offer release
  candidates* (the same switch as in step 3) to include rcs, or pick a version per row (the same as
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

Contact: <kokeblabs@gmail.com>
