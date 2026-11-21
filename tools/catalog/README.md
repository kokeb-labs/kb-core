# compat.yml generator (specification)

Design only. Produces `catalog/compat.yml` (the compatibility catalogue) for a kb-core revision.

## Inputs

`components/drivers/drivers.lock.yml`; the `driver.yml` of each pinned driver at its pinned commit (from the submodule);
`cmake/kb_mcu_gates.cmake` (the gate table); `cmake/kb_version.cmake`; the integration matrix results of the
same run (which configurations compiled, linked and passed).

## Output (schema 1)

```yaml
schema: 1
kb_core: 0.1.0
drivers:
  - name: ws2812_rmt
    version: 0.1.0
    repo: kokeb-labs-drivers/driver-ws2812-rmt
    tag: v0.1.0
    commit: <40 hex>
    bus: [pio]
    mcus: [esp32]
    peripheral: rmt
    kb_core_min: null
    binding: kb/handlers/bindings/ws2812_rmt.hpp
    configs: [{mcu: ESP32, rtos: FREERTOS, result: pass}]
```

## Rules

- Deterministic: sorted by name, no timestamps, so a re-run yields the same bytes.
- A driver whose `mcus`/`peripheral` disagree with the gate table is an error, not an entry.
- CI regenerates the file and fails when it differs from the committed one; the release attaches it.
- Planned home: ci-tools `ru-catalog`, so the configurator and the `kb` CLI share the generator.
