# kb-core

The library between the Kokeb Labs chip drivers and a board HAL. Five parts, each its own CMake target and
package, one version:

| Part | Target | Holds | Used by |
| --- | --- | --- | --- |
| [utils](parts/utils.md) | `kb::utils` | Error model, time units, containers, CRCs | Everyone |
| [rtos](parts/rtos.md) | `kb::rtos` | Sleeps, locks, queues, threads (FreeRTOS, ThreadX or none) | Everyone |
| [dma](parts/dma.md) | `kb::dma` | DMA buffers, leases, cache-policy regions, copy-out streams | Ports, handlers |
| [interfaces](parts/interfaces.md) | `kb::interfaces` | Bus and pin contracts (C++20 concepts), host test port | HAL, handlers, MCU ports |
| [handlers](parts/handlers.md) | `kb::handlers` | Driver bindings, handlers, start-up pipeline, thread models | HAL |

MCU ports (`components/ports/esp32/` first) are separate CMake projects in this repository until they move to their own
repositories ([MCU ports and repositories](design/ports.md)).

> Status: release candidate. [v0.1.0-rc.1](https://github.com/kokeb-labs/kb-core/releases/tag/v0.1.0-rc.1) is the
> first release candidate: the parts, 23 pinned chip drivers and the ESP32 and host ports, tested in CI. APIs may
> still change before 0.1.0.

## In 30 seconds

**[Configure a project](app/index.html)** in the browser: pick an MCU, an RTOS and chip drivers, and download a
pinned project with its SBOMs, licence report and build files ([how it works](configurator.md)). Or build kb-core
itself:

```bash
cmake -S . -B build                     # host: every part, no driver
cmake -S . -B build -DKB_PARTS=utils    # one part alone
```

Start with the [architecture](architecture.md), then the [layering rules](layering.md) and the [plan](design/plan.md).
