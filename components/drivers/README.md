# components/drivers/

The chip drivers kb-core binds, as git submodules `driver-<chip>/` pinned to release-tag commits of
`kokeb-labs-drivers/driver-<chip>` (ADR-0002): every published driver, at v0.1.0-rc.1.

Rules:

- A pin is always the commit of a release tag (`vX.Y.Z`) or release-candidate tag (`vX.Y.Z-rc.N`) whose CI gate
  passed; never `main`, never a branch (ADR-0012). `drivers.lock.yml` names the tag, the commit and the kind, and
  CI checks them against the gitlink and the driver's `driver.yml` (pin hygiene, ADR-0004).
- Never edit a file inside a submodule. A driver change is made in the driver's repository and released there.
- kb-core does not re-run the driver's own tests, fuzzing, mutation, coverage or MISRA scan; it runs
  integration (bindings, configurations, examples) on top of them.
- Init only the drivers you enable, without `--recursive` (the drivers' own tool submodules are not needed):
  `git submodule update --init components/drivers/driver-<chip>`.
- A consumer may point a driver elsewhere with `-DKB_DRIVER_<CHIP>_DIR=<path>`.

A bump is one pull request: gitlink, lock entry, binding and tests, regenerated `catalog/compat.yml`, CHANGELOG.
