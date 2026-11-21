/**
 * @file backend.hpp
 * @brief The seam between kb::rtos's typed API and one RTOS backend: opaque storage and the backend
 * functions.
 *
 * Exactly one backend folder (`components/lib/rtos/backends/<rtos>/`, or `components/ports/host/`
 * for the std::thread test backend) defines these functions; `KB_RTOS` selects it. Application code
 * never calls them: it uses the classes in mutex.hpp, semaphore.hpp, queue.hpp, event_flags.hpp,
 * timer.hpp, thread.hpp and the functions in time.hpp.
 *
 * Each RTOS object lives in an `OpaqueStorage<Bytes>` inside the kb object: bytes sized and aligned
 * for the largest backend's native object (each backend `static_assert`s that its object fits),
 * plus the native handle. No RTOS type appears here.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_RTOS_BACKEND_HPP
#define KB_RTOS_BACKEND_HPP

#include <array>
#include <cstddef>
#include <cstdint>

#include "kb/utils/status.hpp"
#include "kb/utils/time.hpp"

namespace kb::rtos::detail {

/// Alignment of every opaque RTOS object (the strictest of the backends' native objects).
inline constexpr std::size_t kStorageAlign = 16U;
/// Bytes reserved for a mutex.
inline constexpr std::size_t kMutexBytes = 128U;
/// Bytes reserved for a counting semaphore.
inline constexpr std::size_t kSemaphoreBytes = 192U;
/// Bytes reserved for a queue's control block (the item buffer is separate).
inline constexpr std::size_t kQueueBytes = 256U;
/// Bytes reserved for an event-flag group.
inline constexpr std::size_t kEventFlagsBytes = 192U;
/// Bytes reserved for a software timer.
inline constexpr std::size_t kTimerBytes = 256U;
/// Bytes reserved for a thread's control block (the stack is separate).
inline constexpr std::size_t kThreadBytes = 512U;
/// Alignment of a thread stack.
inline constexpr std::size_t kStackAlign = 16U;
/// A queue slot is a whole number of these bytes. The build sets KB_RTOS_QUEUE_WORD_BYTES for a
/// backend that moves messages in native words (ThreadX: `sizeof(ULONG)`, checked by the backend);
/// every other backend copies bytes, so slots are the item size.
#if defined(KB_RTOS_QUEUE_WORD_BYTES)
inline constexpr std::size_t kQueueWordBytes = KB_RTOS_QUEUE_WORD_BYTES;
#else
inline constexpr std::size_t kQueueWordBytes = 1U;
#endif
static_assert(kQueueWordBytes > 0U, "KB_RTOS_QUEUE_WORD_BYTES must be at least 1");

/// Bytes of the item buffer of a queue of `capacity` items of `item_bytes`: each slot rounded up to
/// whole kQueueWordBytes.
[[nodiscard]] constexpr std::size_t QueueBufferBytes(std::size_t item_bytes,
                                                     std::size_t capacity) noexcept {
  return (((item_bytes + kQueueWordBytes) - 1U) / kQueueWordBytes) * kQueueWordBytes * capacity;
}

/**
 * @brief Storage for one native RTOS object: raw bytes and the backend's handle to the object in
 * them.
 * @tparam Bytes Size of the byte area.
 */
template <std::size_t Bytes>
struct alignas(kStorageAlign) OpaqueStorage {
  std::array<std::byte, Bytes> bytes{}; ///< The native object, constructed by the backend's Create.
  void* handle{nullptr};                ///< The backend's handle; null until Create succeeded.
};

using MutexStorage = OpaqueStorage<kMutexBytes>;           ///< A mutex.
using SemaphoreStorage = OpaqueStorage<kSemaphoreBytes>;   ///< A counting semaphore.
using QueueStorage = OpaqueStorage<kQueueBytes>;           ///< A queue control block.
using EventFlagsStorage = OpaqueStorage<kEventFlagsBytes>; ///< An event-flag group.
using TimerStorage = OpaqueStorage<kTimerBytes>;           ///< A software timer.
using ThreadStorage = OpaqueStorage<kThreadBytes>;         ///< A thread control block.

/// A function run by a thread or a timer with its context. Must not throw (there are no
/// exceptions).
using EntryFn = void (*)(void* context) noexcept;

/**
 * @brief The typed link from a backend to a kb thread: the backend runs `run(*core)` on the new
 * thread. A kb::rtos::Thread is a ThreadCore (private base), so no pointer is converted from
 * `void*` above the backends.
 */
struct ThreadCore {
  void (*run)(ThreadCore& core) noexcept {
      nullptr}; ///< The thread's whole life: Setup, loop, Cleanup.
};

/// How a thread is created.
struct ThreadParams {
  ThreadCore* core{nullptr};   ///< Run on the new thread as `core->run(*core)`.
  std::byte* stack{nullptr};   ///< Static stack, aligned to kStackAlign.
  std::size_t stack_bytes{0U}; ///< Size of `stack`.
  const char* name{"kb"};      ///< For the RTOS's diagnostics; may be truncated.
  uint32_t priority{1U}; ///< Backend priority, higher runs first; clamped to the backend's range.
};

/// How a software timer is created.
struct TimerParams {
  EntryFn callback{nullptr}; ///< Run in the timer context at each expiry; must not block.
  void* context{nullptr};    ///< Passed to `callback`.
  uint32_t period_ms{1U};    ///< Period (or delay, one-shot); at least 1.
  bool periodic{true};       ///< Re-arm after each expiry.
};

} // namespace kb::rtos::detail

/// The functions every backend defines (one definition per image).
namespace kb::rtos::backend {

// --- Time
// -----------------------------------------------------------------------------------------------------
/** @brief Scheduler ticks per second. */
[[nodiscard]] uint32_t TickRateHz() noexcept;
/** @brief The scheduler tick counter (wraps at 2^32). */
[[nodiscard]] uint32_t TickCount() noexcept;
/** @brief The 64-bit monotonic clock in microseconds (tick-resolution on a tick-only backend). */
[[nodiscard]] kb::Instant Now() noexcept;
/** @brief Blocks the caller for at least `ticks` scheduler ticks (0 yields). */
void SleepTicks(uint32_t ticks) noexcept;
/**
 * @brief Blocks until `*previous_wake + period_ticks`, then stores that tick in `*previous_wake`.
 * @return False when that tick had already passed (an overrun): the call did not block.
 */
[[nodiscard]] bool SleepUntilTick(uint32_t& previous_wake, uint32_t period_ticks) noexcept;
/** @brief Lets another ready thread of the same priority run. */
void Yield() noexcept;

// --- Critical section
// -----------------------------------------------------------------------------------------
/** @brief Masks interrupts and preemption on this core; nests. */
void EnterCritical() noexcept;
/** @brief Undoes one EnterCritical. */
void ExitCritical() noexcept;

// --- Mutex
// ----------------------------------------------------------------------------------------------------
/** @brief Creates a (non-recursive, priority-inheriting where the RTOS has it) mutex in `storage`.
 */
[[nodiscard]] kb::Status MutexCreate(detail::MutexStorage& storage) noexcept;
/** @brief Takes the mutex, waiting at most `timeout_ticks`. */
[[nodiscard]] bool MutexLock(const detail::MutexStorage& storage, uint32_t timeout_ticks) noexcept;
/** @brief Gives the mutex back; only its holder may call this. */
void MutexUnlock(const detail::MutexStorage& storage) noexcept;

// --- Recursive mutex
// ------------------------------------------------------------------------------------------
/** @brief Creates a recursive, priority-inheriting mutex in `storage`: its holder may take it again
 * and must give it back as many times. */
[[nodiscard]] kb::Status RecursiveMutexCreate(detail::MutexStorage& storage) noexcept;
/** @brief Takes the recursive mutex (again, when the caller holds it), waiting at most
 * `timeout_ticks`. */
[[nodiscard]] bool RecursiveMutexLock(const detail::MutexStorage& storage,
                                      uint32_t timeout_ticks) noexcept;
/** @brief Gives one level of the recursive mutex back; only its holder may call this. */
void RecursiveMutexUnlock(const detail::MutexStorage& storage) noexcept;

// --- Semaphore
// ------------------------------------------------------------------------------------------------
/** @brief Creates a counting semaphore with `max_count` (at least 1) and `initial` (at most max).
 */
[[nodiscard]] kb::Status SemaphoreCreate(detail::SemaphoreStorage& storage, uint32_t max_count,
                                         uint32_t initial) noexcept;
/** @brief Takes one count, waiting at most `timeout_ticks`. */
[[nodiscard]] bool SemaphoreTake(const detail::SemaphoreStorage& storage,
                                 uint32_t timeout_ticks) noexcept;
/** @brief Gives one count; false when already at the maximum. */
[[nodiscard]] bool SemaphoreGive(const detail::SemaphoreStorage& storage) noexcept;
/** @brief Gives one count from an interrupt handler; false when already at the maximum. */
[[nodiscard]] bool SemaphoreGiveFromIsr(const detail::SemaphoreStorage& storage) noexcept;
/** @brief The current count. */
[[nodiscard]] uint32_t SemaphoreCount(const detail::SemaphoreStorage& storage) noexcept;

// --- Queue
// ----------------------------------------------------------------------------------------------------
/** @brief Creates a queue of `capacity` items of `item_bytes` over the caller's `buffer` of
 * `QueueBufferBytes(item_bytes, capacity)` bytes, aligned to kStorageAlign. A backend with a limit
 * on the item size (ThreadX: 16 native words) returns Unsupported above it. */
[[nodiscard]] kb::Status QueueCreate(detail::QueueStorage& storage, std::byte* buffer,
                                     std::size_t item_bytes, std::size_t capacity) noexcept;
/** @brief Copies `item` to the back, waiting at most `timeout_ticks` for room. */
[[nodiscard]] bool QueueSend(const detail::QueueStorage& storage, const void* item,
                             uint32_t timeout_ticks) noexcept;
/** @brief Copies `item` to the back from an interrupt handler; false when full. */
[[nodiscard]] bool QueueSendFromIsr(const detail::QueueStorage& storage, const void* item) noexcept;
/** @brief Moves the front item to `item`, waiting at most `timeout_ticks` for one. */
[[nodiscard]] bool QueueReceive(const detail::QueueStorage& storage, void* item,
                                uint32_t timeout_ticks) noexcept;
/** @brief Items waiting. */
[[nodiscard]] std::size_t QueueSize(const detail::QueueStorage& storage) noexcept;

// --- Event flags
// ----------------------------------------------------------------------------------------------
/** @brief Creates an event-flag group, all flags clear. */
[[nodiscard]] kb::Status EventFlagsCreate(detail::EventFlagsStorage& storage) noexcept;
/** @brief Sets `bits` (24 usable flags: 0x00FFFFFF). */
void EventFlagsSet(const detail::EventFlagsStorage& storage, uint32_t bits) noexcept;
/** @brief Sets `bits` from an interrupt handler; false when the request could not be queued. */
[[nodiscard]] bool EventFlagsSetFromIsr(const detail::EventFlagsStorage& storage,
                                        uint32_t bits) noexcept;
/** @brief Clears `bits`. */
void EventFlagsClear(const detail::EventFlagsStorage& storage, uint32_t bits) noexcept;
/** @brief The flags now. */
[[nodiscard]] uint32_t EventFlagsGet(const detail::EventFlagsStorage& storage) noexcept;
/**
 * @brief Waits at most `timeout_ticks` for any (or all) of `bits`; clears the waited bits on
 * success when asked.
 * @return The flags at the moment the wait ended (test them against `bits`).
 */
[[nodiscard]] uint32_t EventFlagsWait(const detail::EventFlagsStorage& storage, uint32_t bits,
                                      bool wait_all, bool clear_on_exit,
                                      uint32_t timeout_ticks) noexcept;

// --- Timer
// ----------------------------------------------------------------------------------------------------
/** @brief Creates a stopped software timer. */
[[nodiscard]] kb::Status TimerCreate(detail::TimerStorage& storage,
                                     const detail::TimerParams& params) noexcept;
/** @brief Starts (or restarts) the timer. */
[[nodiscard]] bool TimerStart(const detail::TimerStorage& storage) noexcept;
/** @brief Stops the timer; its callback does not run again until a start. */
[[nodiscard]] bool TimerStop(const detail::TimerStorage& storage) noexcept;

// --- Thread
// ---------------------------------------------------------------------------------------------------
/** @brief Creates and starts a thread on its static stack. */
[[nodiscard]] kb::Status ThreadCreate(detail::ThreadStorage& storage,
                                      const detail::ThreadParams& params) noexcept;
/** @brief Called by the thread itself as its very last action: the backend ends the thread. */
void ThreadExit(const detail::ThreadStorage& storage) noexcept;
/** @brief Releases the backend's hold on a thread that has exited (joins it on a host backend). */
void ThreadRelease(detail::ThreadStorage& storage) noexcept;
/**
 * @brief The fewest free bytes the thread's stack has had (its high-water mark), as far as the RTOS
 * has measured.
 * @return Unsupported when the backend or its configuration cannot measure it (no scheduler, a host
 * thread, ThreadX without TX_ENABLE_STACK_CHECKING); NotReady before the thread was created.
 */
[[nodiscard]] kb::Status ThreadStackFree(const detail::ThreadStorage& storage,
                                         std::size_t& free_bytes) noexcept;

} // namespace kb::rtos::backend

#endif // KB_RTOS_BACKEND_HPP
