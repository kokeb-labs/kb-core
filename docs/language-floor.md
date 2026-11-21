# Language floor

Every part of a kb project says which C++ standard it needs at least: kb-core itself, its in-tree ports and every
driver version. This page says where that is written, how a project declares the standard it builds with, and what
the resolver, the configurator and CMake do with both.

## In 30 seconds

- kb-core needs **C++20**, GCC 10 or Clang 13 or newer. So does every driver published today.
- A project may declare its own standard in `kb-image.yml`. Then a part that needs more is refused, with the newest
  version that fits, before anything is downloaded.
- Without that declaration nothing changes: every part is taken at its default version, and CMake builds C++20.

## Where the floor is written

| Part | File | Read by |
| --- | --- | --- |
| A driver, per release | `driver.yml` `language:` at the tag | `kb catalog build`, the configurator's catalogue builder |
| kb-core and its in-tree ports | `kb.yml` `language:` | CMake (`cmake/kb_language.cmake`), both catalogue builders |
| A project | `kb-image.yml` `language:` (optional) | `kb resolve`, `kb upgrade`, the configurator |

The block is the same everywhere; it follows the organisation's standards registry (profiles such as `KB-CPP20`
with their compiler floors):

```yaml
language:
  standard: 20              # the minimum ISO C++ standard
  profile: KB-CPP20         # the handbook profile of that standard
  compilers: {gcc: "10", clang: "13"}   # the oldest compilers it promises to build with (quoted)
  tested: [20]              # every standard CI compiles it under
```

The catalogue copies `standard`, `profile`, `compilers` and `tested` into each version, with `source` saying where
they came from. A tag published before its driver had the block is recorded with the registry default, C++20 and the
KB-CPP20 compiler floors (GCC 10, Clang 13, MSVC 19.30, Arm GCC 10, ESP-IDF 5.0), and `source: default`.

## Declaring the project's standard

```yaml
# kb-image.yml
schema: 1
kb_core: {version: 0.1.0-rc.1}
mcu: {family: esp32, target: esp32c6}
language: {standard: 20, compilers: {gcc: "12"}}   # compilers are optional
drivers: [tmc9660]
```

`standard` is one of 17, 20, 23 and 26; `compilers` may name `gcc`, `clang`, `msvc`, `arm_gcc` and `esp_idf`. In the
configurator, step 2 has a **C++ standard** choice that writes the same block; the version lists of step 3 show what
each version needs (`0.5.2, C++20`).

## What the resolver does

The same rules in `kb resolve`, `kb upgrade` and the configurator (`docs/design/assembler.md`, rule 7), held to the
shared vectors `027` to `032` in `tests/resolver/vectors/`:

| Situation | Result |
| --- | --- |
| A driver named with a version whose floor is above the project's | `KB-RES-024`, with the newest version that fits |
| A driver named without a version, its default above the project's | The newest version that fits is used, warning `KB-RES-W04` |
| The same, and no version fits | `KB-RES-024`, no alternative: raise the project's standard |
| kb-core's floor above the project's | `KB-RES-024`, subject `kb-core`, with the newest kb-core that fits |
| A compiler the project names is older than the part's floor for it | `KB-RES-025`, with the newest version that fits |
| `kb upgrade` | A newer version above the project's floor is held back with the same code |

An example. The catalogue has `tmc9660` 0.6.0 (C++23, GCC 13), 0.5.2 (C++20) and 0.5.0 (no block, so C++20). A
project that declares C++20 and names 0.6.0 gets:

```text
KB-RES-024 tmc9660: tmc9660 0.6.0 needs C++23; the project builds with C++20. tmc9660 0.5.2 is the newest
version that builds with C++20, or raise language.standard to 23
```

The finding carries `alternative: "0.5.2"`, which the configurator shows. The same project naming `tmc9660` without
a version resolves to 0.5.2 with `KB-RES-W04`. A project that declares C++23 and GCC 12 and names 0.6.0 gets
`KB-RES-025` (`gcc 13 or newer`), again with 0.5.2.

The project's standard is what the build uses; a part's `tested` list is evidence that a newer standard also works,
never permission to build a part at a standard below its floor.

## What CMake does

`cmake/kb_language.cmake` reads the floor from `kb.yml`:

- `KB_CPP_STANDARD` (cache) is the standard kb-core builds with. It defaults to the floor and may be 20, 23 or 26.
  A value below the floor stops the configure step with a message that names both (`-DKB_CPP_STANDARD=17` fails;
  the CTest `kb_language_floor` proves it).
- Every kb target requires the floor of its consumers (`cxx_std_20` today); kb-core's own tests, header checks and
  boundary files build at `KB_CPP_STANDARD`, and each header check asserts the matching `__cplusplus`.
- A project whose image declares a language gets `KB_CPP_STANDARD` from its `kb_image.cmake` and its presets.

Raising kb-core's floor is a breaking change and one edit: `standard` in `kb.yml` (CMake, the catalogue and the
resolver follow), plus the changelog entry under **Breaking**.

## Checks

- `tools/kb/tests/test_language.py`: the block reader, the default, a bad block naming every problem, the catalogue
  field, `kb.yml` and CMake reading the same number, and that no library target hard-codes a standard.
- The shared vectors through both resolvers (`kb_cli_unit`, `kb_resolver_js_vectors`).
- The catalogue check (`build_catalog.py --check`) fails when a driver's block at a tag is malformed.

The compiler floors themselves are tested by the organisation's weekly floor-compiler job (GCC 10, Clang 13) in each
driver repository.
