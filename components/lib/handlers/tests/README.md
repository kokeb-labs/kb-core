# components/lib/handlers/tests/

Unit tests of the handler core and the hook bases on the none backend (deterministic time): the start-up runner
stage by stage (sync and polled stages, deadlines, optional stages, failure and retry from the first stage), health
transitions, recovery with backoff, both access facades, the request queue and the seqlock; bus holds and held
calls (`test_hold.cpp`), the asynchronous poller (`test_async_poller.cpp`), parked beats and the liveness verdict,
and the health journal (`test_health_journal.cpp`). `test_threads.cpp` runs on the std::thread host backend:
contention on guarded handlers and buses, lock timeouts, owner requests from several tasks, recovery storms,
seqlock readers against a writer, and held calls under contention. `compile_fail/` proves that a wrong facade, a
wrong bus kind, `Guarded` over a chip that requires an owner, a call without a `ServiceKey`, a plan out of order, a
copied, discarded or temporary `BusHold`, another bus's hold, a guarded `Held()` and a blocking hot-path poller do
not compile. Tests that need a driver are in `tests/integration/`.
