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

> Status: pre-release. The parts and the ESP32 and host ports are in place and tested in CI; no version is tagged
> yet, so APIs may still change.

## In 30 seconds

```bash
cmake -S . -B build                     # host: every part, no driver
cmake -S . -B build -DKB_PARTS=utils    # one part alone
```

Start with the [architecture](architecture.md), then the [layering rules](layering.md) and the [plan](design/plan.md).
