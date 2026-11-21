/**
 * @file queue.hpp
 * @brief kb::rtos::Queue<T, N>: a static-storage FIFO of N copies of T between threads, with an
 * ISR-safe send.
 *
 * Items are copied in and out (T is trivially copyable); the item buffer is inside the queue
 * object, so nothing is allocated. Created explicitly with `Create()`. `SendFor` and `ReceiveFor`
 * wait at most their timeout; `SendFromIsr()` is the only function here an interrupt handler may
 * call.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_RTOS_QUEUE_HPP
#define KB_RTOS_QUEUE_HPP

#include <array>
#include <cstddef>
#include <type_traits>

#include "kb/rtos/backend.hpp"
#include "kb/rtos/time.hpp"
#include "kb/utils/result.hpp"
#include "kb/utils/time.hpp"

namespace kb::rtos {

/** @brief What a Queue carries: a trivially copyable value type. */
template <typename T>
concept QueueItem = std::is_trivially_copyable_v<T> && std::is_default_constructible_v<T>;

/**
 * @brief A bounded FIFO of N items of T, created explicitly.
 * @tparam T Item type (QueueItem).
 * @tparam N Capacity, at least 1.
 */
template <QueueItem T, std::size_t N>
  requires(N > 0U)
class Queue {
public:
  constexpr Queue() noexcept = default;
  Queue(const Queue&) = delete;
  Queue& operator=(const Queue&) = delete;
  Queue(Queue&&) = delete;
  Queue& operator=(Queue&&) = delete;
  ~Queue() = default;

  /** @brief Creates the RTOS queue over the internal buffer. `AlreadyStarted` on a second call. */
  [[nodiscard]] kb::Result<void> Create() noexcept {
    kb::Status status = kb::Status::AlreadyStarted;
    if (!created_) {
      status = backend::QueueCreate(storage_, buffer_.data(), sizeof(T), N);
      created_ = status == kb::Status::Ok;
    }
    return kb::Result<void>::FromStatus(status);
  }

  /** @brief Copies `item` to the back, waiting at most `timeout` for room. */
  [[nodiscard]] bool SendFor(const T& item, Milliseconds timeout) noexcept {
    bool done = false;
    if (created_) {
      done = backend::QueueSend(storage_, &item, ToTicksAtLeast(timeout));
    }
    return done;
  }

  /** @brief Copies `item` to the back if there is room now. */
  [[nodiscard]] bool TrySend(const T& item) noexcept {
    bool done = false;
    if (created_) {
      done = backend::QueueSend(storage_, &item, 0U);
    }
    return done;
  }

  /** @brief Copies `item` to the back from an interrupt handler (ISR-safe); false when full. */
  [[nodiscard]] bool SendFromIsr(const T& item) noexcept {
    bool done = false;
    if (created_) {
      done = backend::QueueSendFromIsr(storage_, &item);
    }
    return done;
  }

  /** @brief Moves the front item into `item`, waiting at most `timeout` for one. */
  [[nodiscard]] bool ReceiveFor(T& item, Milliseconds timeout) noexcept {
    bool done = false;
    if (created_) {
      done = backend::QueueReceive(storage_, &item, ToTicksAtLeast(timeout));
    }
    return done;
  }

  /** @brief Moves the front item into `item` if there is one now. */
  [[nodiscard]] bool TryReceive(T& item) noexcept {
    bool done = false;
    if (created_) {
      done = backend::QueueReceive(storage_, &item, 0U);
    }
    return done;
  }

  /** @brief Items waiting; 0 when not created. */
  [[nodiscard]] std::size_t Size() noexcept {
    std::size_t value = 0U;
    if (created_) {
      value = backend::QueueSize(storage_);
    }
    return value;
  }

  /** @brief The capacity N. */
  [[nodiscard]] static constexpr std::size_t Capacity() noexcept {
    return N;
  }

private:
  static constexpr std::size_t kBufferBytes = detail::QueueBufferBytes(sizeof(T), N);
  detail::QueueStorage storage_{};
  alignas(detail::kStorageAlign) std::array<std::byte, kBufferBytes> buffer_{};
  bool created_{false};
};

} // namespace kb::rtos

#endif // KB_RTOS_QUEUE_HPP
