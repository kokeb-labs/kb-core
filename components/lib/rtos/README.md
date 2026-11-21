# kb::rtos

The only place a scheduler is touched: sleeps that wait at least the time asked, drift-free periodic waits,
mutexes (plain and recursive) with bounded try-lock, semaphores, event flags, static-storage queues, CRTP threads
with static stacks, verified start, park and resume and run-time statistics, and ISR-safe variants.

Rules:

- Public headers are RTOS-free. `KB_RTOS` selects exactly one backend folder in `backends/`; only its `.cpp`
  files are compiled, and only they include RTOS headers (`docs/rtos-boundary.md`).
- Static allocation only (FreeRTOS `...Static` APIs, ThreadX `tx_*_create` on kb-owned blocks). No heap at any
  point. No virtual functions: the thread base
  is `kb::rtos::Thread<Derived, StackBytes>` (ADR-0006).
- Depends on `kb::utils` only. Applications may use it directly.

Backends: `none` (deterministic, no scheduler), `freertos` (boundary files, static allocation), `threadx`
(Eclipse ThreadX, one boundary file, ADR-0015), the test-only `host` backend in `components/ports/host/` (std::thread).

Status: M2 implemented. Guide: `docs/parts/rtos.md`.
