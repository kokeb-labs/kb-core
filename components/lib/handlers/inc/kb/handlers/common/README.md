# components/lib/handlers/inc/kb/handlers/common/

The shared code of every handler: `handler_core.hpp` (`HandlerCore`), `types.hpp` (thread models, bus kinds,
health and start-up types), `health.hpp`, `seqlock.hpp`, `liveness.hpp` (beats, park, the watchdog verdict), `service_key.hpp`,
`bus_domain.hpp` (`BusDomain`, `BusHold`), `request_channel.hpp`, `owner_access.hpp` (`OwnerHeld`), `guarded.hpp`
(`GuardedHeld`), `async_poller.hpp` (`AsyncPoller`), `health_journal.hpp` (`HealthJournal`), `facade.hpp` (the checks both
facades make), `hooks.hpp` (`I2cHooks`, `SpiHooks`, `UartHooks`, `AdcHooks`, `GpioHooks`, `HookTiming`) and
`driver_result.hpp`. Every base class here is a row of the ADR-0006 register.
