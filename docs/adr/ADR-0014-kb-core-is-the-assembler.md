# ADR-0014: kb-core is the assembler

Status: Accepted (owner decision, 2026-11-14). Amends [ADR-0002](ADR-0002-driver-consumption.md),
[ADR-0012](ADR-0012-pins-and-release-lock.md) and [ADR-0013](ADR-0013-hosted-configurator.md).

## Context

kb-core holds four libraries of its own and pins the drivers it binds. Around it, MCU ports move into their own
repositories ([ADR-0008](ADR-0008-repository-naming.md): `kokeb-labs-drivers/port-<family>`), the configurator
site needs the same answers as the command line, and a user wants a driver version that is not the one kb-core
happens to pin (a newer release candidate on a bench, or an older release a product qualified). Three questions
had no single owner:

- Which pieces make up one firmware image, and at which exact commits?
- Which combinations are allowed (MCU gates, RTOS per MCU, `kb_core_min`, one thread model per bus,
  `kRequiresOwner`)?
- How do two front ends, a Python command line and a static JavaScript site, give the same answer forever?

## Decision

1. **kb-core is the assembler.** It is two things:
   - its own libraries, `kb::utils`, `kb::rtos`, `kb::interfaces`, `kb::handlers`, with one version;
   - a **manifest and lock of pinned parts**: driver repositories at release or rc tags
     ([ADR-0012](ADR-0012-pins-and-release-lock.md)), port repositories once extracted, and the in-tree ports
     while they live in kb-core. The manifest is the **catalogue** (`catalog/schemas/catalog.schema.json`): every
     version
     of every part that may be assembled, with its tag, commit and gates. The **lock**
     (`catalog/schemas/lock.schema.json`) is exactly what one image or one release contains.
2. **Versions are selectable per driver.** An image (`kb-image.yml`, `catalog/schemas/kb-image.schema.json`) may name a
   version for each driver and for an extracted port. Without one, the resolver takes kb-core's own pin, else the
   newest release, else the newest release candidate. Only tags in the catalogue are selectable: never a branch,
   never an untagged commit. A release candidate is allowed with warning `KB-RES-W01`.
3. **One resolver specification, one engine per front end.** The rules are written once in
   [Assembler and resolver](../design/assembler.md) and implemented as a pure function of (catalogue, image):
   `tools/kb/resolver.py` for the `kb` command line and `configurator/resolver.mjs` for the site. Neither reads a
   file, a clock or the network while resolving.
4. **Shared vectors prove them identical.** `tests/resolver/vectors/` holds the golden cases: input catalogue and
   image, expected error and warning codes with their subjects, and for a valid image the full lock and the bytes
   of every emitted file. Both implementations run every vector in CI; a seeded property test also feeds the same
   mutated images to both and compares the results. A rule change changes the specification, both
   implementations and the vectors in one pull request.
5. **Errors are structured.** Every refusal is a stable code `KB-RES-NNN` with a subject (the field or the part)
   and a message; warnings are `KB-RES-WNN`. Front ends may word messages differently; codes and subjects never
   differ.
6. **Outputs are repeatable.** For a valid image the resolver emits the lock (`kb-image.lock.yml`), a
   `CMakePresets.json` fragment, `kb_image.cmake` (`KB_MCU`, `KB_RTOS`, `KB_PARTS`, `KB_ENABLE_*`) and
   `kb-fetch.sh`. **One layout**: a user project gets its parts as git submodules under `external/`
   (`external/kb-core`, `external/port-<family>` when extracted, `external/drivers/driver-<chip>`), each checked
   out at its tag, its commit verified against the lock, no branch in `.gitmodules` and no recursion; the fetch
   script creates exactly that, refuses a moved tag, and `--verify` re-checks an existing tree. `kb_image.cmake`
   and the presets point `KB_DRIVER_<CHIP>_DIR` there. `kb sbom` turns a lock into CycloneDX 1.6 and SPDX 2.3
   JSON whose identifiers derive from the lock's SHA-256, so the same lock gives the same bytes; it lists the SDK
   and RTOS (ESP-IDF, FreeRTOS, ThreadX when selected) as SOUP and each component's known-vulnerability scan
   (`kb:osv`), as the site's downloads do.
7. **The catalogue is built, never edited.** `kb catalog build` reads each driver's `driver.yml` at each of its
   `vX.Y.Z` and `vX.Y.Z-rc.N` tags, from local clones with git or from GitHub with `gh api`, plus kb-core's own
   tags and its platform table. The release job publishes the catalogue next to the release lock; the site reads
   those files and nothing else.

### Amended records

- [ADR-0002](ADR-0002-driver-consumption.md): the submodule pins remain kb-core's defaults; an image may select
  another catalogued tag of the same driver.
- [ADR-0012](ADR-0012-pins-and-release-lock.md): the release lock is one instance of the lock schema; an image lock
  has the same shape plus the image's MCU, RTOS, parts, buses and CMake values.
- [ADR-0013](ADR-0013-hosted-configurator.md): "one resolver specification" now names its files: the
  specification page, the two implementations and `tests/resolver/vectors/`.

## Consequences

- One place answers "what is in this image": its lock, by tag and commit, down to every driver.
- A driver release reaches users through the catalogue as soon as it is tagged, without waiting for a kb-core
  pin bump; the bump still decides kb-core's tested default.
- Two implementations must be kept in step. The vectors make drift a failing test, not a silent disagreement.
- Combinations outside kb-core's pins are allowed but not necessarily tested; tested version sets (ADR-0013) stay
  the evidence for that.
- The JSON schemas are a public contract of kb-core: a change to them is a versioned change (`schema: 2`).

## Alternatives considered

| Option | Why not |
| --- | --- |
| Only kb-core's submodule pins, no per-driver choice | Every driver change waits for a kb-core release; benches cannot try an rc |
| A separate assembler repository | A third place for facts kb-core already owns (pins, gates, parts) and another release to coordinate |
| The site calls the Python resolver (a server or WebAssembly) | ADR-0013 requires a static site with no server; shipping a Python runtime to the browser is heavier than one small module |
| One implementation generated from the other | A generator is a third program to trust; two short hand-written implementations plus shared vectors are easier to review |
| Free-form error text | Front ends and tests cannot assert on prose; codes are stable and testable |
