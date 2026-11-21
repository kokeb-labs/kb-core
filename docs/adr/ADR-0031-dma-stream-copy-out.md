# ADR-0031: Streams copy frames out of the DMA ring, and say what they lost

Status: Accepted (owner decision, 2026-11-15). The number is provisional until the record is merged.

## Context

A circular DMA ring fills forever. The earlier implementations let the task read the ring in place, overwrote a held
frame silently, counted an overflow as data, subtracted two unsigned time stamps taken in different contexts, and
let two code paths drain the same flags so a half was delivered twice.

## Decision

1. `Stream<Frame, Frames, Region>` has at least two frames. The interrupt copies each finished hardware frame into a
   slot (`PublishFromDevice`: invalidate the frame's lines, copy out) with a sequence number and the completion time;
   the task takes it with `TryTake` and holds it in a move-only `FrameView`.
2. Three overrun checks: a full ring drops the frame, counts it and flags the next delivered frame `Gap()`; a
   hardware overrun or pool overflow is counted by `OnOverrun`, is never a frame and flags the next one; a copy the
   hardware lapped is published `Torn`.
3. The stall check uses signed elapsed time; a frame stamped after the task read its clock counts as zero elapsed.
   Each stall episode is counted once.
4. One interrupt path reads and clears the hardware flags. There is no second drainer.
5. A content check (`Judge`) feeds a restart policy: `Supervise` returns `Restart`, `BackOff` or `UseFallback`, with a
   new epoch after each. Frames from the declared fallback producer say `Fallback()`.
6. One producer context and one consumer context; counters are atomics and ring indices are single-writer.

## Consequences

- The consumer never sees memory the hardware is writing.
- Loss is visible in the data (gap, torn, fallback, epoch) and in the counters.
- A frame costs one copy in the interrupt; the cost is reported (`NoteIsrCycles`).

## Alternatives considered

- Lending the ring to the task: the held-frame and lapping incidents.
- A lock between interrupt and task: unbounded interrupt latency.
