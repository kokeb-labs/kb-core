/**
 * @file completion.hpp
 * @brief How a transfer ends: the hardware outcome, the proof that a master stopped, and the
 * `CompletionLatch` that makes completion, cancellation and the report happen exactly once.
 *
 * The latch is the one place where an interrupt (completion) and a task (cancel, poll) meet:
 *
 * ```
 *   Idle --Arm--> Pending --TryComplete--> Completed --TakeReport--> Idle
 *                    |                         |
 *                    +-----TryCancel-----------+--> Cancelling --FinishCancel--> Idle
 * ```
 *
 * Whoever moves the latch out of `Pending` first wins; the loser's call returns false and must do
 * nothing. A completion that lost to a cancel is dropped; a cancel that finds an unreported
 * completion drops the completion (the transfer's memory is still returned only after a confirmed
 * stop, see buffer.hpp).
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_DMA_COMPLETION_HPP
#define KB_DMA_COMPLETION_HPP

#include <atomic>
#include <cstdint>

namespace kb::dma {

/** @brief How the hardware finished a transfer. */
enum class Outcome : uint8_t {
  Ok = 0,    ///< The transfer ran to its end.
  Error = 1, ///< A bus error, an overrun or a peripheral error ended it (the DMA may have written
             ///< some bytes).
};

/** @brief Whether the port has proof that the bus master stopped writing the memory. */
enum class StopEvidence : uint8_t {
  NotConfirmed = 0, ///< The stop bit did not read back in its bound, or the SDK cannot say.
  Confirmed = 1,    ///< The enable bit reads 0 (or the SDK reported the transaction finished).
};

/** @brief What `CompletionLatch::TryCancel` found. */
enum class CancelClaim : uint8_t {
  Nothing = 0,          ///< Idle or already cancelling: nothing to cancel, the caller reports Ok.
  CancelledPending = 1, ///< The transfer was in flight; the completion that follows is dropped.
  DroppedCompletion =
      2, ///< The transfer had completed but was not reported; the report is dropped.
};

/** @brief The exactly-once latch of one transfer (see the file note). Lock-free; ISR-safe. */
class CompletionLatch {
public:
  /** @brief Idle to Pending at the start of a transfer; false (unchanged) when not Idle. */
  [[nodiscard]] bool Arm() noexcept {
    uint8_t expected = kIdle;
    return state_.compare_exchange_strong(expected, kPending, std::memory_order_acq_rel,
                                          std::memory_order_acquire);
  }

  /** @brief The completion interrupt: Pending to Completed; false when a cancel got there first. */
  [[nodiscard]] bool TryComplete() noexcept {
    uint8_t expected = kPending;
    return state_.compare_exchange_strong(expected, kCompleted, std::memory_order_acq_rel,
                                          std::memory_order_acquire);
  }

  /** @brief The task's cancel: claims a Pending or an unreported Completed transfer. */
  [[nodiscard]] CancelClaim TryCancel() noexcept {
    CancelClaim claim = CancelClaim::Nothing;
    uint8_t expected = kPending;
    if (state_.compare_exchange_strong(expected, kCancelling, std::memory_order_acq_rel,
                                       std::memory_order_acquire)) {
      claim = CancelClaim::CancelledPending;
    } else {
      expected = kCompleted;
      if (state_.compare_exchange_strong(expected, kCancelling, std::memory_order_acq_rel,
                                         std::memory_order_acquire)) {
        claim = CancelClaim::DroppedCompletion;
      }
    }
    return claim;
  }

  /** @brief Cancelling to Idle once the stop is settled (confirmed or quarantined). */
  void FinishCancel() noexcept {
    uint8_t expected = kCancelling;
    static_cast<void>(state_.compare_exchange_strong(expected, kIdle, std::memory_order_acq_rel,
                                                     std::memory_order_acquire));
  }

  /** @brief The poll: Completed to Idle, true exactly once per completed transfer. */
  [[nodiscard]] bool TakeReport() noexcept {
    uint8_t expected = kCompleted;
    return state_.compare_exchange_strong(expected, kIdle, std::memory_order_acq_rel,
                                          std::memory_order_acquire);
  }

  /** @brief True while a transfer is Pending (in flight and not yet completed or cancelled). */
  [[nodiscard]] bool Pending() const noexcept {
    return state_.load(std::memory_order_acquire) == kPending;
  }

  /** @brief True when the latch is Idle. */
  [[nodiscard]] bool Idle() const noexcept {
    return state_.load(std::memory_order_acquire) == kIdle;
  }

  /** @brief True when a completion waits to be reported. */
  [[nodiscard]] bool CompletionWaiting() const noexcept {
    return state_.load(std::memory_order_acquire) == kCompleted;
  }

private:
  static constexpr uint8_t kIdle = 0U;
  static constexpr uint8_t kPending = 1U;
  static constexpr uint8_t kCompleted = 2U;
  static constexpr uint8_t kCancelling = 3U;

  std::atomic<uint8_t> state_{kIdle};
};

} // namespace kb::dma

#endif // KB_DMA_COMPLETION_HPP
