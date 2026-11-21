# ADR-0002: Driver consumption

Status: Accepted (owner decision, 2026-11-14). Pin rules sharpened by [ADR-0012](ADR-0012-pins-and-release-lock.md);
the pins are part of the assembler manifest of [ADR-0014](ADR-0014-kb-core-is-the-assembler.md).

## Context

Drivers are separate repositories with their own CI and release tags. Pinning them through an aggregate
repository that is itself a submodule of the core makes one driver change take two or three pull requests with a
merge order and a re-pin after merge. kb-core must also serve ESP-IDF, host and later STM32 builds, and its
configuration must be reproducible and archivable.

## Decision

Drivers are **git submodules** under `components/drivers/driver-<chip>/`, each pinned to the commit of a release tag or
release-candidate tag whose CI passed, never a branch ([ADR-0012](ADR-0012-pins-and-release-lock.md)).
`components/drivers/drivers.lock.yml` records repository, tag and commit; CI checks it against the gitlink and the driver's
`driver.yml` ([ADR-0004](ADR-0004-ci-strategy.md)). `KB_DRIVER_<CHIP>_DIR` lets a consumer supply a driver from
elsewhere (its own checkout, a FetchContent result, an ESP-IDF managed component). Only enabled drivers are
initialised, without `--recursive`.

## Consequences

- A driver bump is one kb-core pull request. The pin is a commit in kb-core's own history, auditable and
  buildable offline.
- Consumers run one more command (`git submodule update --init ...`); the configurator writes it for them
  ([ADR-0013](ADR-0013-hosted-configurator.md)).
- Registry publication (ESP-IDF component manager) remains possible on top, without changing kb-core.

## Alternatives considered

| Option | Why not the default |
| --- | --- |
| Pinned FetchContent | Network at configure time, cache handling inside ESP-IDF, pin in CMake text instead of git history |
| ESP-IDF component manifest | ESP-IDF only; host and STM32 need another path; every driver must publish first |
| Vendored copies | A driver fix would be made twice, and the copy drifts from the released driver |
