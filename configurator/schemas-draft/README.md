# Draft schemas

Drafts of the three contracts between the configurator and the `kb` tool, written here because kb-core had no
`catalog/schemas/` folder when the configurator was built. When kb-core publishes its own schemas, these files move
there
(or are replaced by them) and the configurator validates against kb-core's copies.

| Schema | Describes | Validated by |
| --- | --- | --- |
| `catalog.schema.json` | `data/catalog.json`: kb-core releases, MCU families, targets, boards, RTOS backends, bus kinds, drivers with every version, tested sets | `tools/build_catalog.py` on every build; `tests/schemas.test.mjs` |
| `kb-image.schema.json` | The selection the resolver takes (the JSON form of `kb-image.yml`) | `tests/schemas.test.mjs` for every vector |
| `resolver-vector.schema.json` | One golden test vector: catalogue, selection, expected findings and resolved build | `tests/schemas.test.mjs` |

The rule codes the vectors name are specified in `docs/configurator-resolver.md`.
