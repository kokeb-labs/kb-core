# catalog/

`compat.yml`: which driver versions this kb-core version binds, on which MCU and RTOS configurations, with which
gate. It is **generated** in CI from the `driver.yml` of every pinned driver plus kb-core's gate table and
integration results, never edited by hand; CI fails when the committed file differs from the generated one.
Every release attaches it, next to the release lock and the evidence files (ADR-0012). The `kb` tool, `kb-image.yml`
and the hosted configurator (ADR-0013) read it. After a port moves out, it also holds a kb-core x port row.

Generator spec: `tools/catalog/README.md`.
