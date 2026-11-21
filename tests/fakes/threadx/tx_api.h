// A fake of the ThreadX API that kb-core's ThreadX backend calls, for host link-seam tests of the
// boundary file (components/lib/rtos/backends/threadx/). Single-threaded and deterministic; see
// fake_threadx.hpp for its control. Not ThreadX: only the names, types and documented results of
// the calls kb-core makes (the real kernel is tested with the ThreadX Linux port,
// components/lib/rtos/tests/README.md). SPDX-License-Identifier: MIT Copyright (c) 2026 The kb-core
// authors
#ifndef KB_FAKE_TX_API_H
#define KB_FAKE_TX_API_H

#include <cstdint>

using VOID = void;
using CHAR = char;
using UINT = unsigned int;
using ULONG = uint32_t;

#define TX_SUCCESS (static_cast<UINT>(0x00))
#define TX_DELETED (static_cast<UINT>(0x01))
#define TX_NO_EVENTS (static_cast<UINT>(0x07))
#define TX_QUEUE_EMPTY (static_cast<UINT>(0x0A))
#define TX_QUEUE_FULL (static_cast<UINT>(0x0B))
#define TX_NO_INSTANCE (static_cast<UINT>(0x0D))
#define TX_NOT_AVAILABLE (static_cast<UINT>(0x1D))
#define TX_NOT_OWNED (static_cast<UINT>(0x1E))
#define TX_CEILING_EXCEEDED (static_cast<UINT>(0x21))
#define TX_THREAD_ERROR (static_cast<UINT>(0x0E))
#define TX_NO_WAIT (static_cast<ULONG>(0))
#define TX_WAIT_FOREVER (static_cast<ULONG>(0xFFFFFFFFUL))
#define TX_AND (static_cast<UINT>(2))
#define TX_AND_CLEAR (static_cast<UINT>(3))
#define TX_OR (static_cast<UINT>(0))
#define TX_OR_CLEAR (static_cast<UINT>(1))
#define TX_NO_TIME_SLICE (static_cast<ULONG>(0))
#define TX_AUTO_START (static_cast<UINT>(1))
#define TX_NO_ACTIVATE (static_cast<UINT>(0))
#define TX_NULL nullptr
#define TX_INHERIT (static_cast<UINT>(1))
#define TX_INT_DISABLE 1U
#define TX_INT_ENABLE 0U
#define TX_STACK_FILL (static_cast<ULONG>(0xEFEFEFEFUL))
#define TX_MAX_PRIORITIES 32
#define TX_TIMER_THREAD_PRIORITY 0
#define TX_MINIMUM_STACK 200
#define TX_TIMER_TICKS_PER_SECOND (1000UL)
// The tick the fake kernel is "built" for, as a real build passes it (KB_THREADX_TICK_HZ).
#if !defined(KB_THREADX_TICK_HZ)
#define KB_THREADX_TICK_HZ 1000UL
#endif
#define TX_ENABLE_STACK_CHECKING

struct TX_THREAD {
  VOID* tx_thread_stack_start{nullptr};
  VOID* tx_thread_stack_end{nullptr};
  ULONG tx_thread_stack_size{0U};
  VOID* tx_thread_stack_highest_ptr{nullptr};
  VOID (*entry)(ULONG){nullptr};
  ULONG entry_input{0U};
  const CHAR* name{nullptr};
  UINT priority{0U};
  UINT preempt_threshold{0U};
  ULONG time_slice{0U};
  UINT auto_start{0U};
  uint32_t index{0U};
};
struct TX_MUTEX {
  TX_THREAD* owner{nullptr};
  ULONG count{0U};
  UINT inherit{0U};
};
struct TX_SEMAPHORE {
  ULONG count{0U};
};
struct TX_QUEUE {
  ULONG* start{nullptr};
  UINT words{0U};
  ULONG capacity{0U};
  ULONG head{0U};
  ULONG count{0U};
};
struct TX_EVENT_FLAGS_GROUP {
  ULONG flags{0U};
};
struct TX_TIMER {
  VOID (*function)(ULONG){nullptr};
  ULONG input{0U};
  ULONG remaining{0U};
  ULONG reschedule{0U};
  bool active{false};
};

UINT tx_interrupt_control(UINT new_posture);
ULONG tx_time_get();
TX_THREAD* tx_thread_identify();
VOID tx_thread_relinquish();
UINT tx_thread_sleep(ULONG timer_ticks);
UINT tx_thread_create(TX_THREAD* thread_ptr, CHAR* name_ptr, VOID (*entry_function)(ULONG),
                      ULONG entry_input, VOID* stack_start, ULONG stack_size, UINT priority,
                      UINT preempt_threshold, ULONG time_slice, UINT auto_start);
UINT tx_thread_terminate(TX_THREAD* thread_ptr);
UINT tx_thread_delete(TX_THREAD* thread_ptr);

UINT tx_mutex_create(TX_MUTEX* mutex_ptr, CHAR* name_ptr, UINT inherit);
UINT tx_mutex_get(TX_MUTEX* mutex_ptr, ULONG wait_option);
UINT tx_mutex_put(TX_MUTEX* mutex_ptr);
UINT tx_mutex_info_get(TX_MUTEX* mutex_ptr, CHAR** name, ULONG* count, TX_THREAD** owner,
                       TX_THREAD** first_suspended, ULONG* suspended_count, TX_MUTEX** next_mutex);

UINT tx_semaphore_create(TX_SEMAPHORE* semaphore_ptr, CHAR* name_ptr, ULONG initial_count);
UINT tx_semaphore_get(TX_SEMAPHORE* semaphore_ptr, ULONG wait_option);
UINT tx_semaphore_ceiling_put(TX_SEMAPHORE* semaphore_ptr, ULONG ceiling);
UINT tx_semaphore_info_get(TX_SEMAPHORE* semaphore_ptr, CHAR** name, ULONG* current_value,
                           TX_THREAD** first_suspended, ULONG* suspended_count,
                           TX_SEMAPHORE** next_semaphore);

UINT tx_queue_create(TX_QUEUE* queue_ptr, CHAR* name_ptr, UINT message_size, VOID* queue_start,
                     ULONG queue_size);
UINT tx_queue_send(TX_QUEUE* queue_ptr, VOID* source_ptr, ULONG wait_option);
UINT tx_queue_receive(TX_QUEUE* queue_ptr, VOID* destination_ptr, ULONG wait_option);
UINT tx_queue_info_get(TX_QUEUE* queue_ptr, CHAR** name, ULONG* enqueued, ULONG* available_storage,
                       TX_THREAD** first_suspended, ULONG* suspended_count, TX_QUEUE** next_queue);

UINT tx_event_flags_create(TX_EVENT_FLAGS_GROUP* group_ptr, CHAR* name_ptr);
UINT tx_event_flags_set(TX_EVENT_FLAGS_GROUP* group_ptr, ULONG flags_to_set, UINT set_option);
UINT tx_event_flags_get(TX_EVENT_FLAGS_GROUP* group_ptr, ULONG requested_flags, UINT get_option,
                        ULONG* actual_flags_ptr, ULONG wait_option);
UINT tx_event_flags_info_get(TX_EVENT_FLAGS_GROUP* group_ptr, CHAR** name, ULONG* current_flags,
                             TX_THREAD** first_suspended, ULONG* suspended_count,
                             TX_EVENT_FLAGS_GROUP** next_group);

UINT tx_timer_create(TX_TIMER* timer_ptr, CHAR* name_ptr, VOID (*expiration_function)(ULONG),
                     ULONG expiration_input, ULONG initial_ticks, ULONG reschedule_ticks,
                     UINT auto_activate);
UINT tx_timer_activate(TX_TIMER* timer_ptr);
UINT tx_timer_deactivate(TX_TIMER* timer_ptr);
UINT tx_timer_change(TX_TIMER* timer_ptr, ULONG initial_ticks, ULONG reschedule_ticks);

#endif
