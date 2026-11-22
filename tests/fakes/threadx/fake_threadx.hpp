// Control of the fake ThreadX kernel (tests/fakes/threadx/): its tick clock, created threads and
// timers, the calling thread, failure injection and call counters. Test code only.
// SPDX-License-Identifier: MIT Copyright (c) 2026 The kb-core authors
#ifndef KB_FAKE_THREADX_CONTROL_HPP
#define KB_FAKE_THREADX_CONTROL_HPP

#include <cstddef>
#include <cstdint>

#include "tx_api.h"

namespace kb_fake_threadx {

void Reset();             ///< Forgets every object and counter; tick 0; the test thread runs.
void SetTick(ULONG tick); ///< Sets the tick counter (no timers fire).
void AdvanceTicks(uint32_t ticks); ///< Advances the clock tick by tick, firing due timers.
void FailNextCall();
void FailNextThreadCreate(); ///< Only the next tx_thread_create fails.
void FailNextActivate(); ///< Only the next tx_timer_activate fails.                  ///< The next
                         ///< create, activate or change returns an error.
std::size_t ThreadCount();            ///< Threads created since Reset().
TX_THREAD* Thread(std::size_t index); ///< Thread `index` as created.
void RunThread(
    std::size_t index); ///< Runs thread `index`'s entry to its end, as the calling thread.
void SetCurrent(TX_THREAD* thread);     ///< The calling thread; null: not a thread (an ISR).
TX_THREAD* TestThread();                ///< The thread the test body runs as.
uint32_t Terminates();                  ///< tx_thread_terminate calls.
uint32_t Deletes();                     ///< tx_thread_delete calls.
uint32_t Relinquishes();                ///< tx_thread_relinquish calls.
void SetSleepOvershoot(uint32_t ticks); ///< The next tx_thread_sleep wakes this much late.
uint32_t Sleeps();                      ///< tx_thread_sleep calls.
UINT Posture();              ///< The interrupt posture now (TX_INT_DISABLE or TX_INT_ENABLE).
std::size_t TimerCount();    ///< Timers created since Reset().
uint32_t SemaphoreCreates(); ///< Semaphores created since Reset().
uint32_t TimersHeld();       ///< Timers created and not deleted by this process, across Reset().
uint32_t ObjectDeletes(); ///< tx_*_delete calls of mutexes, semaphores, queues, flags and timers.
TX_TIMER* Timer(std::size_t index); ///< Timer `index` as created; null once deleted.

} // namespace kb_fake_threadx

#endif
