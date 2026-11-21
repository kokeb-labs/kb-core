# kb::interfaces

The contracts of buses, pins, clocks and logs as C++20 concepts, and only concepts (`I2cBus`, `SpiDevice`,
`UartPort`, `GpioOutput`, `GpioInput`, `AdcChannel`, `CanBus`, `PulseTx`, `MonotonicClock`, `BusyWait`,
`LogSink`; ADR-0006), the vocabulary types they use, type-erased `*Ref` values where a HAL needs a run-time choice,
and the platform-free host test port in `host/`.

Rules:

- **Used only by the HAL, `kb::handlers` and the MCU ports.** Applications never include `kb/interfaces/...` nor
  link `kb::interfaces`; a HAL links it PRIVATE. `tools/include_checker` and the CMake graph check enforce it.
- Contracts (`inc/`) are platform-free and depend only on `kb::rtos` and `kb::utils`. No virtual functions.
- MCU code is not here: each MCU family is its own CMake project in `components/ports/<mcu>/` (ADR-0007).
- Every port has a conformance test (`static_assert` on the concept) and the host port has fault injection.

Status: M3 implemented. Guide: `docs/parts/interfaces.md`; contracts derived from the drivers' ports:
`docs/port-contracts.md`.
