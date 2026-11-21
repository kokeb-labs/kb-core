# ADR-0004: CI strategy, what kb-core runs and what it trusts

Status: Accepted (owner decision, 2026-11-14). Assurance level set by [ADR-0011](ADR-0011-assurance-level-high.md) (`high`).

## Context

Each driver passes its own full CI before a release tag: host tests, coverage, sanitizers, fuzzing, MISRA,
clang-tidy, cppcheck, complexity, handbook check, cross and ESP32 builds, plus mutation runs and independent
review in its pipeline. Re-running all of that in kb-core for every driver would cost CI minutes and add no
evidence about the same commit.

## Decision

kb-core runs:

1. **Its own parts at full rigor** for level `high` (ADR-0011): host tests with two compilers, clang-tidy and
   cppcheck, coverage gates, sanitizers, fuzzing, complexity, mutation testing, MISRA C++:2023 with KB-CPP20 on
   host and Cortex-M, standalone build of each part and install consumers.
2. **Integration**: compile and link of every binding over every port and allowed MCU/RTOS configuration; host
   tests that drive each driver through its kb binding with the driver's chip model; the ESP-IDF build of the
   ESP32 port and its examples, as the port's own job ([ADR-0007](ADR-0007-mcu-ports-isolated.md)).
3. **Pin hygiene**: each pin equals a release or release-candidate tag whose CI gate passed, and that tag's
   `driver.yml` agrees with the lock file, the MCU gate table and this kb-core version
   ([ADR-0012](ADR-0012-pins-and-release-lock.md)).
4. The include and dependency checker, the poison build and the symbol lint; `catalog/compat.yml` generation;
   docs; DCO and org standard checks.
5. **Release evidence** on every tag: the release lock, the SBOM, the known-CVE scan and the licence report
   ([ADR-0012](ADR-0012-pins-and-release-lock.md), [ADR-0013](ADR-0013-hosted-configurator.md)).

kb-core **does not re-run for driver content**: driver unit tests, coverage, mutation, fuzzing, MISRA,
clang-tidy, cppcheck, complexity, handbook and docs checks. kb-core's analysers restrict their reported paths to
`components/lib/` (and each port's job to its own folder). The trust is conditional: pin hygiene is the proof that the
pinned
commit is the one that passed.

Every check is a thin caller of a `kokeb-labs/ci-tools` reusable workflow. New reusable workflows needed in
ci-tools: `ru-mutation`, `ru-cmake-matrix`, `ru-pin-hygiene`, `ru-layering`, `ru-catalog`, `ru-release-evidence`.

## Consequences

- CI time is spent on kb-core's own code and on the joints between kb-core and drivers, where new defects live.
- A pin to an untested commit or a branch cannot pass. A driver whose CI was weaker than required at its tag is
  caught by the handbook level check of pin hygiene.
- Until the pending reusable workflows exist, those checks run locally only; their caller files are present but
  not called from `ci.yml`.

## Alternatives considered

Re-running driver CI inside kb-core (double cost, same evidence); trusting pins without checking (an untested or
retagged commit could slip in).
