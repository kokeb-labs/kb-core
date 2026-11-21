# ADR-0013: A hosted configurator that hands out repeatable, pinned projects with evidence

Status: Accepted (owner decision, 2026-11-13). Amended by [ADR-0014](ADR-0014-kb-core-is-the-assembler.md): the
site and the `kb` CLI share one resolver specification and its vectors in `tests/resolver/vectors/`.

## Context

A user of the kb family has to choose an MCU family, an RTOS, a kb-core version, drivers and their handlers, and
then get exactly those, pinned, in a form that builds the same way next year. They also need evidence for what
they took: what is in it (SBOM), which known vulnerabilities affect it, which licences apply, which combinations
were actually tested together. The catalogue (`catalog/compat.yml`), the release lock
([ADR-0012](ADR-0012-pins-and-release-lock.md)) and tested version sets already hold the facts; what is missing is
an easy front end.

## Decision

1. **A static configurator website**, hosted on GitHub Pages first: plain HTML, CSS and JavaScript with no server,
   no account, no cookies, no analytics and no third-party script at run time (dependencies are vendored and
   pinned, a strict Content Security Policy is set). The skeleton lives in kb-core `configurator/` now and moves to
   the `kb-configurator` repository ([ADR-0008](ADR-0008-repository-naming.md)) when it needs its own release
   cadence.
2. **Inputs**, all published by kb-core releases and read as static JSON: the catalogue (MCU gates, bindings,
   `kb_core_min`, thread-model constraints), the release lock of each kb-core version, the evidence files of each
   release, and the tested version sets (`kb YYYY.N`).
3. **The user picks** MCU family and target, RTOS backend, kb-core version or a tested set, drivers, the handlers
   to bind, and the thread model of each bus. The site resolves the selection with the same rules as the `kb` CLI
   (drivers to handlers to interfaces to parts, MCU gates, `kb_core_min`, one thread model per bus,
   `kRequiresOwner`); both front ends implement one resolver specification and must pass the same golden test
   vectors, so they cannot disagree.
4. **Downloads**, byte-for-byte repeatable (sorted, no timestamps; the scan date is a separate field):
   - `kb-image.yml`, the selection itself;
   - the git commands: clone kb-core at its tag, check the commit SHA against the release lock, init only the
     enabled submodules without `--recursive`, and check each submodule's SHA;
   - a `CMakePresets.json` fragment and `kb_image.cmake` (`KB_MCU`, `KB_RTOS`, `KB_PARTS`, `KB_ENABLE_*`);
   - later, an archive of a ready project made from `kb-app-template` with the same pins.
5. **Evidence for the selection**, filtered from the release's evidence to exactly the chosen components:
   - an SBOM in **SPDX 2.3** and **CycloneDX 1.6** JSON (kb-core parts, drivers, port, SOUP, with versions,
     licences, package URLs and SHAs);
   - **known-CVE scan results** for those components (scanned in the release job, re-scanned on a schedule so a
     new CVE shows against an old release; each result carries its scan date and database version);
   - a **licence report** (each component's SPDX licence and copyright holder);
   - the **compatibility rows** of the selection from the catalogue;
   - the **tested version set** the selection belongs to and the configurations that passed, or a clear
     "untested combination" warning.
6. kb-core's release job produces the evidence files (pending ci-tools `ru-release-evidence`); the site never
   generates evidence itself.
7. Phases: P0 skeleton page (now); P1 at M4 (catalogue, `kb-image.yml`, git commands, presets); P2 at M6 (SBOM,
   CVE results, licence report, tested sets); P3 at M7 (project archive, `board.yml` pin tables).

## Consequences

- One place to get a pinned, explainable project; the same selection always yields the same files.
- The evidence is release evidence, signed by the release's provenance, not something the site computes.
- The resolver exists twice (Python CLI, JavaScript site), held together by shared test vectors.
- A static site cannot fetch sources across origins; P1 and P2 give commands and evidence, and the archive waits for
  P3.

## Alternatives considered

| Option | Why not |
| --- | --- |
| A server-side generator | Hosting, accounts and an attack surface for something a static page can do |
| CLI only | Harder first contact; the CLI stays for automation |
| ESP-IDF component manifests only | ESP-IDF only, and no evidence |
