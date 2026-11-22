# tests/

Repository-wide tests; each part's own unit tests are in `components/lib/<part>/tests/`.

| Folder | What it proves |
| --- | --- |
| `integration/` | Every driver works through its kb binding over the host ports and the driver's chip model |
| `layering/` | The include checker accepts the good fixtures and rejects each bad one |
| `poison/` | Platform-free code builds and behaves the same with SDK macros defined and no SDK include path |
| `standalone/` | Each part configures and builds with only itself and its declared dependencies |
| `install_consumer/` | Each installed package works through `find_package(kb COMPONENTS ...)` |
| `port_sdk/` | A minimal port built from the extracted port SDK archive alone, running the DMA conformance suite |
| `docs/` | Every code block of the documentation compiles and runs |
