# components/lib/rtos/backends/none/

The backend for builds without a scheduler (host tests, bare-metal bring-up): locks are no-ops that still check
their own use, sleeps advance the fake clock, queues are single-context. Must be deterministic: every host test
of the parts above runs on it.
