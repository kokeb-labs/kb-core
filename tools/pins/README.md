# Pin hygiene check (specification)

Design only. Rules: ADR-0002 and ADR-0012. For each submodule under `components/drivers/` (and, after extraction, each
pinned
port):

| Id | Check |
| --- | --- |
| PIN-001 | `drivers.lock.yml` has exactly one entry per submodule and none without one |
| PIN-002 | The gitlink commit equals the lock entry's `commit` |
| PIN-003 | The lock entry's `tag` exists in the driver repository and points at that commit (annotated tags dereferenced) |
| PIN-004 | The tag is a release or release-candidate tag (`^v\d+\.\d+\.\d+(-rc\.\d+)?$`) and the lock's `kind` agrees (`release` or `rc`); a final kb-core release with an rc pin is a warning |
| PIN-005 | The driver's required check "CI gate / gate" concluded `success` on that commit |
| PIN-006 | `driver.yml` at the commit: `name` equals the lock name, `version` equals the tag without `v` |
| PIN-007 | `mcus` and `peripheral` equal kb-core's gate table entry (no entry means `mcus: [any]`, `peripheral: null`) |
| PIN-008 | `kb_core_min` is null or not above this kb-core version |
| PIN-009 | `.kokeb/handbook.yml` at the commit declares level `high` or `safety` |
| PIN-010 | The submodule working tree is clean and the `.gitmodules` URL is `https://github.com/kokeb-labs-drivers/driver-<chip>.git` |
| PIN-011 | `.gitmodules` has no `branch =` entry, and no gitlink points at a commit that is only on a branch (never `main`) |
| PIN-012 | At a release tag: `kb-core-<version>.lock.yml` is complete, names this tag's own commit SHA, and every commit in it equals the tree being released |

Read-only GitHub API calls with the workflow token (a read token for private drivers). Output: one line per
finding, a step summary table of every pin, exit 1 on any finding. Planned home: ci-tools `ru-pin-hygiene`.

## Release lock

The release job writes `kb-core-<version>.lock.yml` (format: ADR-0012): kb-core's version, tag and own commit SHA,
every pin with repository, tag, commit and kind, the in-repository ports, the toolchain and SOUP versions CI used,
and the SHA-256 of every release file. It is a release file, listed in `SHA256SUMS` and covered by the build
provenance; PIN-012 checks it before the release is published.
