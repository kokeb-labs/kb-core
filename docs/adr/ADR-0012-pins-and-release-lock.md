# ADR-0012: Pins to release or release-candidate tags only; every release records its own SHA and a lock

Status: Accepted (owner decision, 2026-11-13). Sharpens [ADR-0002](ADR-0002-driver-consumption.md). Amended by
[ADR-0014](ADR-0014-kb-core-is-the-assembler.md): the release lock is one form of the assembler lock.

## Context

A submodule can point at any commit, and `.gitmodules` can ask for a branch. A pin to a branch tip or to an
untagged commit is a pin to code no CI gate ever approved as a release. A kb-core release also has to say exactly
what it contains, all the way down, for users who must rebuild it years later and for the configurator's
evidence ([ADR-0013](ADR-0013-hosted-configurator.md)). A tag name alone is not enough: tags can move.

## Decision

1. Every submodule (drivers now, extracted ports later) is pinned to the commit of a tag named `vX.Y.Z` or
   `vX.Y.Z-rc.N` in that repository, whose required CI check passed on that commit. **Never `main`, never a branch,
   never an untagged commit.** `.gitmodules` carries no `branch =` entry.
2. `components/drivers/drivers.lock.yml` records, per pin: `name`, `repo`, `tag`, `commit` and `kind` (`release` or
   `rc`).
3. A kb-core release may contain release-candidate pins; each is listed in the release notes and in the release
   lock, and pin hygiene warns on a final kb-core release that contains one.
4. **Each kb-core release records its own commit SHA and a lock file.** The release job writes
   `kb-core-<version>.lock.yml`, attached to the release, listed in `SHA256SUMS` and covered by the build
   provenance:

   ```yaml
   schema: 1
   kb_core: {version: 0.4.0, tag: v0.4.0, commit: <40 hex>}
   drivers:
     - {name: pf1550, repo: kokeb-labs-drivers/driver-pf1550, tag: v0.1.0, commit: <40 hex>, kind: release}
   ports:
     - {name: esp32, path: components/ports/esp32, commit: <the kb-core commit>}   # after extraction: repo, tag, commit
   toolchain: {cmake: <version>, gcc: <version>, clang: <version>, esp_idf: v5.5.x, codeql_misra_pack: <version>}
   soup: [{name: freertos-kernel, version: <version>, via: esp-idf}]
   files: [{name: kb_utils-0.4.0.tar.gz, sha256: <64 hex>}]
   ```

   The lock cannot live in the tagged commit (it would change the SHA it records); it is a release file, and the
   tag's commit is the source of truth it points at.
5. The pin hygiene checks stay and grow: PIN-001 to PIN-010 ([tools/pins](../../tools/pins/README.md)),
   plus PIN-011 (no `branch =` in `.gitmodules`; the tag matches `^v\d+\.\d+\.\d+(-rc\.\d+)?$`; `kind` agrees with
   the tag) and PIN-012 (at release: the lock is complete and every commit in it equals the tree being released).

## Consequences

- Any kb-core release can be rebuilt from its lock alone, and a moved tag is detected by its SHA.
- A driver change reaches kb-core only after the driver tagged it and its CI passed.
- Release candidates can flow through kb-core's own pre-releases without waiting for final driver tags.
- Consumers' version locks (handbook ADR-0007) reference kb-core by tag and SHA, the same pair the release lock
  records.

## Alternatives considered

| Option | Why not |
| --- | --- |
| Pins to `main` or a branch | No release behind it; the pin changes meaning when the branch moves |
| Final tags only | Blocks kb-core pre-releases while drivers are pre-1.0 |
| Lock committed after the release | A second commit per release, and the lock would not be in the release's own provenance |
