# ADR-0017: A kb::rtos object's destructor deletes its native object

Status: Accepted (2026-11-21)

## Context

Every `kb::rtos` object (`Mutex`, `RecursiveMutex`, `Semaphore`, `Queue`, `EventFlags`, `Timer`) builds its native
RTOS object in opaque storage inside itself (`detail::OpaqueStorage`). Until now the backend seam had a create call
for each kind but no delete, and every destructor was `= default`: a native object was never ended. Only `Thread`
had a release hook (`ThreadRelease`).

That was harmless only while every object had static storage. Tests, and any code that keeps an object on a stack
frame or in an object that ends, then build new native objects in bytes whose old native object was never ended:

- **Host backend.** A `std::mutex` and a `std::condition_variable` were constructed over earlier ones that were never
  destroyed, so POSIX never saw `pthread_mutex_destroy` or `pthread_cond_destroy` before a new init at the same
  address. On macOS, `kb_handlers_threads` hung now and then under load, always in the test
  "recoveries asked by one task while another task's requests run, through the owner task". Five stack samples
  (three given, one from a local repro, one from a plain preflight with no load generator) show the same state:
  every thread that uses the channel's event-flag mutex sleeps in `__psynch_mutexwait`, and no live thread holds
  that mutex. In the plain-preflight sample the owner task (`EventFlags::Set`) and the requester (re-locking after
  its timed wait) both sleep on the mutex and nothing spins. No kb code path can leave that mutex held: every lock
  is a short RAII scope with no nested lock. The test's teardown hang (`~Thread` joining for ever) follows from it:
  `Finish()` and `Stop()` time out after 60 s, then the destructor joins a thread that never wakes.
- **Host timers.** A `HostTimer`'s thread outlived its `Timer`, so the callback went on running on storage that had
  ended (a deterministic failure: a 1 ms timer fired 26 times after its end in the new regression test, then the
  process aborted).
- **ThreadX.** A created object stays on a kernel list; an object on a stack frame left a dangling entry, and a
  timer kept its slot of the backend's 32-slot table for ever.
- **none.** A timer stayed in the fake clock's table, which kept calling into ended storage.

## Decision

1. The backend seam (`kb/rtos/backend.hpp`) gains one call per kind: `MutexDelete`, `RecursiveMutexDelete`,
   `SemaphoreDelete`, `QueueDelete`, `EventFlagsDelete`, `TimerDelete`. Each is called once, by the kb object's
   destructor, only after a successful create, and clears `storage.handle`. The caller's contract: nothing holds,
   waits on or uses the object any more (the usual C++ lifetime rule, now stated).
2. Each destructor calls its delete when the object was created; a never-created object deletes nothing.
3. Per backend:

   | Backend | Mutex, semaphore, queue | Event flags | Timer |
   | --- | --- | --- | --- |
   | host | destructor of the placement-new'd standard-library object | destructor | destructor, which ends and joins the timer's thread |
   | FreeRTOS (and ESP32) | `vQueueDelete` (`vSemaphoreDelete` is that call) | `vEventGroupDelete` | documented no-op |
   | ThreadX | `tx_mutex_delete`, `tx_semaphore_delete`, `tx_queue_delete` | `tx_event_flags_delete` | `tx_timer_delete`, and its table slot is freed |
   | none | no-op (plain values) | no-op | leaves the fake clock's table |

   The FreeRTOS timer is not deleted: `xTimerDelete` only queues a command for the timer task, which may still use
   the control block after the kb object's storage has ended, so a synchronous delete does not exist there.
4. Misuse is defined on the host backend: an object that another thread still holds (mutex) or waits on (semaphore,
   queue, event flags: a waiter count under the object's own mutex) is left in place, never destroyed under that
   thread; a timer deleted from its own callback is left running, as its thread cannot join itself. The handle is
   dropped either way.

## Consequences

- No native object is ever built over one that was not ended, on any backend with a delete.
- Static objects are unchanged (their destructors run at exit, if at all). Objects on a stack frame or inside an
  ending object are now correct on ThreadX and the host, and need no slot of the ThreadX timer table after their end.
- A FreeRTOS `Timer` still needs static storage, as before; the parts page says so.
- Regression tests: `kb_rtos_tests_*` build each kind again and again in the same bytes over every backend; the
  host suite runs groups and semaphores in shared bytes under contention, checks that a timer never fires after its
  end, and checks the refusals of item 4. The CTest timeouts (`KB_TEST_TIMEOUT`) turn any hang left into a failure
  in minutes.
- Not proven: the hang has no deterministic reproduction. Before the fix it showed in about 1 run of 100 of the
  whole suite under load; the evidence that the fix removes it is a run count, recorded in the pull request.

## Alternatives considered

- **Heap-allocate host objects and never free them** (no address is ever reused). Smallest change, but it leaks every
  object, LeakSanitizer reports it on Linux, and it leaves the ThreadX, none and host-timer problems.
- **Keep the seam; make the host backend detect a re-created address.** It cannot tell a new object from an old one
  at the same address without a delete, so it would guess.
- **Only document "static storage for every object".** The rule already existed and tests broke it; nothing checks
  it, and the host timer and ThreadX table problems show the cost of relying on it.
- **A delete in the FreeRTOS timer via `xTimerDelete` with a wait.** Still asynchronous: the timer task may touch the
  control block after the command is queued, so it would trade a documented limit for a hidden use-after-end.
