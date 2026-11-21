/**
 * @file request_channel.hpp
 * @brief kb::handlers::RequestChannel<CommandT, ReplyT, Depth>: the bounded command queue between
 * the tasks that call an owner-bus handler and its owner task, with a completion per request
 * (ADR-0009, model O `Request(cmd, timeout)`).
 *
 * Depth slots, each with an atomic state; a kb::rtos::Queue of slot indices (capacity Depth, so it
 * never fills while a slot is free) and a kb::rtos::EventFlags bit per slot for the completion.
 *
 * | State | Set by | Next |
 * | --- | --- | --- |
 * | Free | owner or caller | Claimed (a caller takes it) |
 * | Claimed | caller (fills the request) | Posted |
 * | Posted | caller (index queued) | Running (owner takes it) or Abandoned (caller timed out:
 * nothing was sent) | | Running | owner | Done (owner completed) or Abandoned (caller timed out
 * while it ran) | | Done | owner (reply written, flag set) | Free (caller read the reply) | |
 * Abandoned | caller | Free (owner skips or completes it) |
 *
 * A caller never waits for a slot (`Busy` when none is free) and waits at most its timeout for the
 * completion (`Timeout`). The owner frees every abandoned slot, so no slot is lost while the owner
 * runs. All payload fields are written before a release store of the state and read after an
 * acquire load, so the states order every access.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_HANDLERS_COMMON_REQUEST_CHANNEL_HPP
#define KB_HANDLERS_COMMON_REQUEST_CHANNEL_HPP

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "kb/handlers/common/seqlock.hpp"
#include "kb/rtos/event_flags.hpp"
#include "kb/rtos/queue.hpp"
#include "kb/rtos/time.hpp"
#include "kb/utils/result.hpp"
#include "kb/utils/status.hpp"
#include "kb/utils/time.hpp"

namespace kb::handlers {

/** @brief What a request asks the owner task to do. */
enum class RequestKind : uint8_t {
  Command = 0, ///< Run the handler's command and reply.
  Sample = 1,  ///< Run the periodic read now and publish the snapshot.
  Start = 2,   ///< Run the start-up plan; completes when it finished or failed.
  Recover = 3, ///< Run Recover(); completes when the restart finished or failed.
  Stop = 4,    ///< Deinitialize the handler.
};

/** @brief The life of a request slot (see the file note). */
enum class SlotState : uint8_t {
  Free = 0,
  Claimed = 1,
  Posted = 2,
  Running = 3,
  Done = 4,
  Abandoned = 5,
};

/** @brief One request slot: the state and the payload it orders. */
template <typename CommandT, typename ReplyT>
struct RequestSlot {
  std::atomic<uint8_t> state{static_cast<uint8_t>(SlotState::Free)}; ///< A SlotState.
  CommandT command{};                                                ///< The request.
  ReplyT reply{};                                                    ///< The reply (Done).
  uint32_t detail{0U};                                               ///< The reply's detail.
  kb::Status status{kb::Status::Ok};                                 ///< The reply's status.
  RequestKind kind{RequestKind::Command};                            ///< What was asked.
};

/** @brief A request as the owner task takes it from the channel. */
template <typename CommandT>
struct TakenRequest {
  CommandT command{};                     ///< The request.
  uint32_t slot{0U};                      ///< Its slot, for Complete().
  RequestKind kind{RequestKind::Command}; ///< What was asked.
};

/** @brief Counters of a RequestChannel. */
struct ChannelStats {
  uint32_t posted{0U};    ///< Requests posted.
  uint32_t busy{0U};      ///< Posts refused: no free slot.
  uint32_t timeouts{0U};  ///< Waits that timed out (the slot was abandoned).
  uint32_t completed{0U}; ///< Replies delivered to a waiting caller.
  uint32_t skipped{0U};   ///< Abandoned requests the owner freed without running.
};

/**
 * @brief The request queue of one owner-bus handler.
 * @tparam CommandT The request payload (trivially copyable).
 * @tparam ReplyT The reply payload (trivially copyable).
 * @tparam Depth Slots: 1 to 24 (one event flag each).
 */
template <SnapshotValue CommandT, SnapshotValue ReplyT, std::size_t Depth>
  requires((Depth >= 1U) && (Depth <= 24U))
class RequestChannel {
public:
  constexpr RequestChannel() noexcept = default;
  RequestChannel(const RequestChannel&) = delete;
  RequestChannel& operator=(const RequestChannel&) = delete;
  RequestChannel(RequestChannel&&) = delete;
  RequestChannel& operator=(RequestChannel&&) = delete;
  ~RequestChannel() = default;

  /** @brief Creates the queue and the flags (after the scheduler runs). */
  [[nodiscard]] kb::Result<void> Create() noexcept {
    kb::Result<void> result = queue_.Create();
    if (result.IsOk()) {
      result = flags_.Create();
    }
    return result;
  }

  /// @name Caller side (any task but the owner)
  /// @{

  /** @brief Posts a request. @return The slot to Wait() on; Busy when every slot is in use. */
  [[nodiscard]] kb::Result<uint32_t> Post(RequestKind kind, const CommandT& command) noexcept {
    kb::Result<uint32_t> result = kb::Result<uint32_t>::Failure(kb::Status::Busy);
    bool placed = false;
    for (std::size_t i = 0U; i < Depth; ++i) {
      bool claimed = false;
      if (!placed) {
        claimed = claim(slots_[i]);
      }
      if (claimed) {
        RequestSlot<CommandT, ReplyT>& s = slots_[i];
        s.kind = kind;
        s.command = command;
        flags_.Clear(bitOf(i));
        s.state.store(static_cast<uint8_t>(SlotState::Posted), std::memory_order_release);
        const auto index = static_cast<uint8_t>(i);
        placed = queue_.TrySend(index);
        if (placed) {
          static_cast<void>(posted_.fetch_add(1U, std::memory_order_relaxed));
          result = kb::Result<uint32_t>::Success(static_cast<uint32_t>(i));
        } else {
          s.state.store(static_cast<uint8_t>(SlotState::Free), std::memory_order_release);
        }
      }
    }
    if (!placed) {
      static_cast<void>(busy_.fetch_add(1U, std::memory_order_relaxed));
    }
    return result;
  }

  /**
   * @brief Waits at most `timeout` for the reply of `slot` (from Post()).
   * @return The reply; Timeout when the owner did not complete in time (the request is abandoned:
   * not sent when the owner had not taken it yet); InvalidArgument for a bad slot.
   */
  [[nodiscard]] kb::Result<ReplyT> Wait(uint32_t slot, kb::Milliseconds timeout) noexcept {
    kb::Result<ReplyT> result = kb::Result<ReplyT>::Failure(kb::Status::InvalidArgument);
    if (slot < Depth) {
      RequestSlot<CommandT, ReplyT>& s = slots_[slot];
      const uint32_t bit = bitOf(slot);
      const kb::Instant start = kb::rtos::Now();
      bool done = false;
      bool waiting = true;
      while (waiting) {
        const uint64_t elapsed_ms = kb::utils::ElapsedUs(kb::rtos::Now(), start) / kUsPerMs;
        const uint64_t total_ms = timeout.Count();
        const kb::Milliseconds left{
            static_cast<uint32_t>((elapsed_ms >= total_ms) ? 0U : (total_ms - elapsed_ms))};
        const bool flagged = (flags_.WaitAny(bit, left, true) & bit) != 0U;
        done = isDone(s);
        // A flag without Done is a late flag of an earlier request on this slot: wait on.
        waiting = flagged && (!done) && (left.Count() != 0U);
      }
      if (!done) {
        done = !abandon(s);
        if (done) {
          // Completed between the wait and the abandon: its flag follows the Done at once;
          // consume it so it cannot wake the next request posted on this slot.
          static_cast<void>(flags_.WaitAny(bit, kLateFlagBound, true));
        }
      }
      if (done) {
        result = take(s);
      } else {
        static_cast<void>(timeouts_.fetch_add(1U, std::memory_order_relaxed));
        result = kb::Result<ReplyT>::Failure(kb::Status::Timeout);
      }
    }
    return result;
  }

  /// @}
  /// @name Owner side (the owner task only)
  /// @{

  /**
   * @brief Takes the next posted request; abandoned ones are freed on the way.
   * @return False when nothing is waiting.
   */
  [[nodiscard]] bool Next(TakenRequest<CommandT>& out) noexcept {
    bool found = false;
    uint8_t index = 0U;
    for (std::size_t n = 0U; n < Depth; ++n) {
      bool got = false;
      if (!found) {
        got = queue_.TryReceive(index);
      }
      if (got && (index < Depth)) { // GCOVR_EXCL_BR_LINE: the queue holds only indices Post() wrote
        RequestSlot<CommandT, ReplyT>& s = slots_[index];
        auto expected = static_cast<uint8_t>(SlotState::Posted);
        if (s.state.compare_exchange_strong(expected, static_cast<uint8_t>(SlotState::Running),
                                            std::memory_order_acq_rel)) {
          out.command = s.command;
          out.kind = s.kind;
          out.slot = index;
          found = true;
        } else {
          s.state.store(static_cast<uint8_t>(SlotState::Free), std::memory_order_release);
          static_cast<void>(skipped_.fetch_add(1U, std::memory_order_relaxed));
        }
      }
    }
    return found;
  }

  /** @brief Completes `slot` (from Next()) with `reply`; frees it when the caller gave up. */
  void Complete(uint32_t slot, const kb::Result<ReplyT>& reply) noexcept {
    if (slot < Depth) {
      RequestSlot<CommandT, ReplyT>& s = slots_[slot];
      s.status = reply.Error();
      s.detail = reply.Detail();
      s.reply = reply.IsOk() ? reply.Value() : ReplyT{};
      auto expected = static_cast<uint8_t>(SlotState::Running);
      if (s.state.compare_exchange_strong(expected, static_cast<uint8_t>(SlotState::Done),
                                          std::memory_order_acq_rel)) {
        flags_.Set(bitOf(slot));
      } else {
        s.state.store(static_cast<uint8_t>(SlotState::Free), std::memory_order_release);
      }
    }
  }

  /// @}

  /** @brief A copy of the counters. */
  [[nodiscard]] ChannelStats Stats() const noexcept {
    ChannelStats st{};
    st.posted = posted_.load(std::memory_order_relaxed);
    st.busy = busy_.load(std::memory_order_relaxed);
    st.timeouts = timeouts_.load(std::memory_order_relaxed);
    st.completed = completed_.load(std::memory_order_relaxed);
    st.skipped = skipped_.load(std::memory_order_relaxed);
    return st;
  }

  /** @brief The state of `slot` (tests and diagnostics). */
  [[nodiscard]] SlotState StateOf(uint32_t slot) const noexcept {
    return (slot < Depth)
               ? static_cast<SlotState>(slots_[slot].state.load(std::memory_order_acquire))
               : SlotState::Free;
  }

  /** @brief Slots. */
  [[nodiscard]] static constexpr std::size_t Capacity() noexcept {
    return Depth;
  }

private:
  static constexpr uint64_t kUsPerMs = 1000U;
  /// How long a caller that saw Done without the flag waits for that flag (it follows at once).
  static constexpr kb::Milliseconds kLateFlagBound{10U};

  [[nodiscard]] static bool isDone(const RequestSlot<CommandT, ReplyT>& s) noexcept {
    return s.state.load(std::memory_order_acquire) == static_cast<uint8_t>(SlotState::Done);
  }

  [[nodiscard]] static constexpr uint32_t bitOf(std::size_t slot) noexcept {
    return (uint32_t{1U} << static_cast<uint32_t>(slot));
  }

  [[nodiscard]] static bool claim(RequestSlot<CommandT, ReplyT>& s) noexcept {
    auto expected = static_cast<uint8_t>(SlotState::Free);
    return s.state.compare_exchange_strong(expected, static_cast<uint8_t>(SlotState::Claimed),
                                           std::memory_order_acq_rel);
  }

  // Gives up a request that is not Done: true when abandoned, false when it completed meanwhile.
  [[nodiscard]] static bool abandon(RequestSlot<CommandT, ReplyT>& s) noexcept {
    auto posted = static_cast<uint8_t>(SlotState::Posted);
    auto running = static_cast<uint8_t>(SlotState::Running);
    const auto abandoned = static_cast<uint8_t>(SlotState::Abandoned);
    bool gave_up = s.state.compare_exchange_strong(posted, abandoned, std::memory_order_acq_rel);
    if (!gave_up) {
      gave_up = s.state.compare_exchange_strong(running, abandoned, std::memory_order_acq_rel);
    }
    return gave_up;
  }

  // Reads the reply of a Done slot and frees it.
  [[nodiscard]] kb::Result<ReplyT> take(RequestSlot<CommandT, ReplyT>& s) noexcept {
    kb::Result<ReplyT> result = kb::Result<ReplyT>::Failure(kb::Status::Fault);
    if (s.state.load(std::memory_order_acquire) == static_cast<uint8_t>(SlotState::Done)) {
      result = (s.status == kb::Status::Ok) ? kb::Result<ReplyT>::Success(s.reply)
                                            : kb::Result<ReplyT>::Failure(s.status, s.detail);
      s.state.store(static_cast<uint8_t>(SlotState::Free), std::memory_order_release);
      static_cast<void>(completed_.fetch_add(1U, std::memory_order_relaxed));
    }
    return result;
  }

  std::array<RequestSlot<CommandT, ReplyT>, Depth> slots_{};
  kb::rtos::Queue<uint8_t, Depth> queue_{};
  kb::rtos::EventFlags flags_;
  std::atomic<uint32_t> posted_{0U};
  std::atomic<uint32_t> busy_{0U};
  std::atomic<uint32_t> timeouts_{0U};
  std::atomic<uint32_t> completed_{0U};
  std::atomic<uint32_t> skipped_{0U};
};

} // namespace kb::handlers

#endif // KB_HANDLERS_COMMON_REQUEST_CHANNEL_HPP
