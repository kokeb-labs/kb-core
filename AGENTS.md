# AGENTS.md

kb-core is the Kokeb Labs library between the chip drivers (`kokeb-labs-drivers/driver-<chip>`) and the board
HALs: `kb::utils`, `kb::rtos`, `kb::interfaces` and `kb::handlers`, one version, plus MCU ports in
`components/ports/<mcu>/`
that are their own CMake projects. It follows the
[Kokeb Labs engineering standard](https://github.com/kokeb-labs/.github/blob/main/standards/README.md) and the
handbook at level `high`, declared in `.kokeb/handbook.yml` (ADR-0011).

## Layout

Where things live is named once, in `tools/layout/paths.yml`. CMake (`cmake/kb_layout.cmake`: `kb_layout(lib VAR)`,
`KB_LIB_DIR`, ...), the tools, `tools/preflight.sh` and the tests read it; none hard-codes a location, so another
grouping is a one-line change there (steps in the file's header). Prose, workflow files and YAML carry the literal
path: `tools/layout/layout.py stale` fails on a retired one and `stale --fix` rewrites it. `layout.py root`
fails on a top-level entry that is not in the allowlist in the same file (preflight, ctest, `ci-layout.yml`).

- `components/lib/<part>/`: one part each, `inc/kb/<part>/*.hpp` public headers, `src/*.ipp` implementations,
  `tests/`, `fuzz/`. Boundary `.cpp` files only in `components/lib/rtos/backends/<rtos>/`. The host test doubles
  are `components/lib/interfaces/host/`.
- `components/ports/<mcu>/`: one MCU family port each, its own CMake project, its own `deviations/`; it uses kb-core
  only through installed public headers (ADR-0007). Never add the MCU ports from kb-core's CMake.
  `components/ports/host/` is the exception: the host port (the std::thread RTOS backend), built in kb-core's project.
- `components/drivers/`: driver submodules pinned to release or rc tags, `drivers.lock.yml`. Never edit inside a
  submodule.
- `examples/`: first-class. Real programs built in CI and run as tests, shown in the docs by snippet includes (never
  pasted), listed in `examples/examples.yml` (`tools/examples/examples.py check`).
- `catalog/compat.yml`: generated in CI; never edit by hand. `catalog/schemas/`: the JSON schemas.
- `cmake/`: `kb_build_settings.cmake` (`KB_MCU`, `KB_RTOS`, `KB_PARTS`, `KB_ENABLE_<DRIVER>`), the MCU gate
  (`kb_mcu_gates.cmake`); `kb_version.cmake` is the only place the version is set.
- `deviations/`: MISRA records and the boundary file list of kb-core.
- `configurator/`: the hosted configurator (ADR-0013).
- `tools/`: `layout/` (the layout file and its checks), `examples/`, `kb/` (the `kb` tool), `lint/`, `preflight*`,
  specifications of the include checker, the catalogue generator and the pin check.
- `docs/`: the site, design pages (`docs/design/`), ADRs (`docs/adr/`), figure sources (`docs/diagrams/`),
  assurance records (`docs/assurance/`).
- `_config/`: all tool configuration; pass the path, never copy it to the root. `.kokeb/`, `.github/`: org conventions.

## Rules

- Write code fresh under the pipeline (design, build, self-review, gate, independent review, fix). Never port or
  copy code from older stacks (ADR-0005).
- Dependencies point down only: `utils <- rtos <- interfaces <- handlers` (ADR-0001). Applications never use
  `kb::interfaces` or `kb::handlers`.
- No SDK or RTOS header outside the files in a `boundary-files.yml`; no `#if` on a platform macro above them; no
  board or product value anywhere in kb-core.
- C++20 only, KB-CPP20 profile: no exceptions, no RTTI, no heap, no virtual functions; contracts are concepts only;
  a base class only where the ADR-0006 register lists it.
- Handlers: single-threaded core, one thread model per bus (ADR-0009), start-up as a declared plan (ADR-0010).
- Pins only to `vX.Y.Z` or `vX.Y.Z-rc.N` tags, never a branch (ADR-0012).
- kb-core trusts each pinned driver's own CI and does not re-run its tests, fuzzing, mutation, coverage or MISRA
  scan; it runs integration and pin hygiene (ADR-0004).
- Every change: tests, docs and `CHANGELOG.md` in the same pull request; commits signed off (`git commit -s`);
  no attribution lines.
- Ask the maintainer before anything irreversible or public: releases, visibility, creating repositories,
  deleting branches or tags.
