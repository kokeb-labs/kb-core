/*
 * kb_tx_user.h: the ThreadX configuration (tx_user.h) kb-core builds the pinned kernel with, for
 * its own tests and cross-compiles (components/lib/rtos/backends/threadx/README.md). A product
 * passes its own tx_user.h; it must keep the options marked "required" below.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_TX_USER_H
#define KB_TX_USER_H

/* Required: the tick kb-core converts milliseconds with, set on purpose (the build passes
   KB_THREADX_TICK_HZ to the kernel and the backend, and the backend checks they agree). It must
   equal the rate the port's tick interrupt really runs at (on Cortex-M the SysTick reload in
   tx_initialize_low_level.S). */
#if !defined(KB_THREADX_TICK_HZ)
#error                                                                                             \
    "kb_tx_user.h: build with KB_THREADX_TICK_HZ (components/lib/rtos/backends/threadx/CMakeLists.txt)"
#endif
#define TX_TIMER_TICKS_PER_SECOND (KB_THREADX_TICK_HZ)

/* Recommended: stack checking (and the stack filling it implies), for Thread::StackFreeBytes() and
   ThreadX's own overflow detection. Without it StackFreeBytes() returns Unsupported. */
#define TX_ENABLE_STACK_CHECKING

/* Required: not defined, kb::rtos needs timers and timeouts (TX_NO_TIMER), and kb::rtos::Timer
   callbacks may run in the timer thread or, with TX_TIMER_PROCESS_IN_ISR, in the tick interrupt:
   both are "timer context". */

#endif
