/**
 * @file mutex.hpp
 * @brief kb::rtos::Mutex and kb::rtos::RecursiveMutex with a bounded try-lock, and the RAII
 * LockGuard and RecursiveLockGuard that hold them for one scope.
 *
 * A mutex is constructed inert (constexpr, no RTOS call) and becomes usable with `Create()`, called
 * after the scheduler runs. Every lock is bounded: `TryLockFor(timeout)` waits at most `timeout`
 * (at least that long when it fails) and says whether it got the lock. Locking before `Create()`
 * fails.
 *
 * @code
 * kb::rtos::Mutex bus_lock{};
 * if (!bus_lock.Create().IsOk()) { ... }
 * {
 *   kb::rtos::LockGuard guard{bus_lock, kb::Milliseconds{5U}};
 *   if (!guard.OwnsLock()) { return kb::Status::Busy; }
 *   ... // exclusive access; unlocked at the end of the scope
 * }
 * @endcode
 *
 * A RecursiveMutex may be taken again by its holder (each take needs its own unlock); prefer Mutex,
 * whose second take by the holder fails instead of hiding a re-entry.
 *
 * Both priority-inherit where the RTOS has it (FreeRTOS mutexes, ThreadX `TX_INHERIT`). Not
 * callable from an interrupt handler.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_RTOS_MUTEX_HPP
#define KB_RTOS_MUTEX_HPP

#include "kb/rtos/backend.hpp"
#include "kb/rtos/time.hpp"
#include "kb/utils/result.hpp"
#include "kb/utils/time.hpp"

namespace kb::rtos {

/** @brief A non-recursive mutex (priority inheriting where the RTOS has it), created explicitly. */
class Mutex {
public:
  constexpr Mutex() noexcept = default;
  Mutex(const Mutex&) = delete;
  Mutex& operator=(const Mutex&) = delete;
  Mutex(Mutex&&) = delete;
  Mutex& operator=(Mutex&&) = delete;
  ~Mutex() = default;

  /** @brief Creates the RTOS object. `AlreadyStarted` on a second call. */
  [[nodiscard]] kb::Result<void> Create() noexcept {
    kb::Status status = kb::Status::AlreadyStarted;
    if (!created_) {
      status = backend::MutexCreate(storage_);
      created_ = status == kb::Status::Ok;
    }
    return kb::Result<void>::FromStatus(status);
  }

  /** @brief Takes the mutex if it is free now. */
  [[nodiscard]] bool TryLock() noexcept {
    bool done = false;
    if (created_) {
      done = backend::MutexLock(storage_, 0U);
    }
    return done;
  }

  /**
   * @brief Takes the mutex, waiting at most `timeout`; false (after at least `timeout`) when it
   * stayed held. The holder asking again is a bug: it fails after the timeout, but an unbounded
   * wait (`kb::kMaxMilliseconds`) differs by RTOS: it fails at once on ThreadX and never returns on
   * FreeRTOS.
   */
  [[nodiscard]] bool TryLockFor(Milliseconds timeout) noexcept {
    bool done = false;
    if (created_) {
      done = backend::MutexLock(storage_, ToTicksAtLeast(timeout));
    }
    return done;
  }

  /** @brief Gives the mutex back. Precondition: the caller holds it. */
  void Unlock() noexcept {
    if (created_) {
      backend::MutexUnlock(storage_);
    }
  }

  /** @brief True after a successful Create(). */
  [[nodiscard]] bool IsCreated() const noexcept {
    return created_;
  }

private:
  detail::MutexStorage storage_{};
  bool created_{false};
};

/** @brief Holds a Mutex for one scope, if it could be taken within the timeout. */
class LockGuard {
public:
  /** @brief Tries to take `mutex` within `timeout`; check OwnsLock(). */
  LockGuard(Mutex& mutex, Milliseconds timeout) noexcept
      : mutex_(mutex), owns_(mutex.TryLockFor(timeout)) {}
  LockGuard(const LockGuard&) = delete;
  LockGuard& operator=(const LockGuard&) = delete;
  LockGuard(LockGuard&&) = delete;
  LockGuard& operator=(LockGuard&&) = delete;
  /** @brief Unlocks when the lock was taken. */
  ~LockGuard() {
    if (owns_) {
      mutex_.Unlock();
    }
  }

  /** @brief True when the guard holds the mutex. */
  [[nodiscard]] bool OwnsLock() const noexcept {
    return owns_;
  }

private:
  Mutex& mutex_;
  bool owns_;
};

/** @brief A recursive mutex (priority inheriting where the RTOS has it), created explicitly. */
class RecursiveMutex {
public:
  constexpr RecursiveMutex() noexcept = default;
  RecursiveMutex(const RecursiveMutex&) = delete;
  RecursiveMutex& operator=(const RecursiveMutex&) = delete;
  RecursiveMutex(RecursiveMutex&&) = delete;
  RecursiveMutex& operator=(RecursiveMutex&&) = delete;
  ~RecursiveMutex() = default;

  /** @brief Creates the RTOS object. `AlreadyStarted` on a second call. */
  [[nodiscard]] kb::Result<void> Create() noexcept {
    kb::Status status = kb::Status::AlreadyStarted;
    if (!created_) {
      status = backend::RecursiveMutexCreate(storage_);
      created_ = status == kb::Status::Ok;
    }
    return kb::Result<void>::FromStatus(status);
  }

  /** @brief Takes the mutex (again, when the caller holds it) if that is possible now. */
  [[nodiscard]] bool TryLock() noexcept {
    bool done = false;
    if (created_) {
      done = backend::RecursiveMutexLock(storage_, 0U);
    }
    return done;
  }

  /** @brief Takes the mutex, waiting at most `timeout`; false (after at least `timeout`) when
   * another thread kept it. */
  [[nodiscard]] bool TryLockFor(Milliseconds timeout) noexcept {
    bool done = false;
    if (created_) {
      done = backend::RecursiveMutexLock(storage_, ToTicksAtLeast(timeout));
    }
    return done;
  }

  /** @brief Gives one level back. Precondition: the caller holds it. */
  void Unlock() noexcept {
    if (created_) {
      backend::RecursiveMutexUnlock(storage_);
    }
  }

  /** @brief True after a successful Create(). */
  [[nodiscard]] bool IsCreated() const noexcept {
    return created_;
  }

private:
  detail::MutexStorage storage_{};
  bool created_{false};
};

/** @brief Holds one level of a RecursiveMutex for one scope, if it could be taken within the
 * timeout. */
class RecursiveLockGuard {
public:
  /** @brief Tries to take `mutex` within `timeout`; check OwnsLock(). */
  RecursiveLockGuard(RecursiveMutex& mutex, Milliseconds timeout) noexcept
      : mutex_(mutex), owns_(mutex.TryLockFor(timeout)) {}
  RecursiveLockGuard(const RecursiveLockGuard&) = delete;
  RecursiveLockGuard& operator=(const RecursiveLockGuard&) = delete;
  RecursiveLockGuard(RecursiveLockGuard&&) = delete;
  RecursiveLockGuard& operator=(RecursiveLockGuard&&) = delete;

  /** @brief Gives the level back when it was taken. */
  ~RecursiveLockGuard() {
    if (owns_) {
      mutex_.Unlock();
    }
  }

  /** @brief True when the guard holds a level of the mutex. */
  [[nodiscard]] bool OwnsLock() const noexcept {
    return owns_;
  }

private:
  RecursiveMutex& mutex_;
  bool owns_;
};

} // namespace kb::rtos

#endif // KB_RTOS_MUTEX_HPP
