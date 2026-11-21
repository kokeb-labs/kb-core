/**
 * @file model_port.hpp
 * @brief The reference port over the host model: cache-maintenance hooks, and a one-channel DMA
 * port that follows every rule of kb::dma, with a switch (`Faults`) for each defect the conformance
 * suite must catch.
 *
 * `ModelPort` is what a real port does, in the order it does it: lend the buffer (maintenance by
 * `Buffer::Lend`), start the engine, let the interrupt service the flags (individually cleared,
 * completion claimed through the `CompletionLatch`), report once from `Poll`, abort with a bounded
 * wait for the enable bit to read 0 and quarantine when it does not, recover only after the
 * peripheral was reset. Every deliberate defect in `Faults` removes exactly one of those steps.
 *
 * Test code, outside the MISRA claim.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_DMA_HOST_MODEL_PORT_HPP
#define KB_DMA_HOST_MODEL_PORT_HPP

#include <cstddef>
#include <cstdint>
#include <span>

#include "kb/dma/buffer.hpp"
#include "kb/dma/capabilities.hpp"
#include "kb/dma/completion.hpp"
#include "kb/dma/host/engine.hpp"
#include "kb/dma/host/machine.hpp"
#include "kb/dma/region.hpp"
#include "kb/utils/status.hpp"
#include "kb/utils/time.hpp"

namespace kb::dma::host {

/**
 * @brief The deliberate defects of the broken models. All false is the correct port.
 *
 * Each one is the defect class of a real incident; the conformance case named in the comment must
 * fail when it is set (`test_conformance.cpp` checks that every case does).
 */
struct Faults {
  bool skip_clean{false};            ///< T1: the clean hook does nothing.
  bool skip_invalidate_all{false};   ///< T2: the invalidate hook does nothing.
  bool skip_invalidate_after{false}; ///< T2: invalidate at the lend only, not at the completion.
  bool cpu_path_invalidates{false};  ///< T3: a CPU-copy transfer invalidates after the copy.
  bool shared_lines{false};          ///< T4: the receive target is a raw span sharing a line.
  bool view_during_lease{false};     ///< T5: the CPU gets a view while the lease is out.
  bool timeout_returns_early{false}; ///< T6: a timeout returns the buffer without stopping the DMA.
  bool abort_no_wait{false};         ///< T7: an abort does not wait for EN to read 0.
  bool unpadded_staging{false};      ///< T8: the staging area is exactly the frame, unpadded.
  bool two_drainers{false};          ///< T9/T11: a task also drains the stream flags.
  bool ignore_lapped{false};         ///< T10: both flags pending is published as clean.
  bool global_flag_clear{false};     ///< T14: the interrupt clears all flags at once.
  bool report_ok_on_error{false};    ///< T14: a transfer error is reported as Ok.
  bool double_report{false}; ///< T15: a completion that lost to a cancel is reported anyway.
  bool unconfirmed_returns_cpu{false}; ///< T16: an unconfirmed abort returns the buffer to the CPU.
  bool silent_fallback{false};         ///< T17: the port reports DMA while serving by interrupt.
  bool dma_into_caller{false};         ///< T18: the DMA targets the caller's own buffer.
};

/** @brief The port's cache hooks over the machine; honours `skip_clean` and the invalidate faults.
 */
class ModelCacheOps {
public:
  /** @brief Hooks on `machine` with the defects in `faults`. */
  ModelCacheOps(Machine& machine, const Faults& faults) noexcept
      : machine_(machine), faults_(faults) {}

  /** @brief Clean hook. */
  void Clean(std::span<const std::byte> range) noexcept {
    if (!faults_.skip_clean) {
      machine_.CleanRange(range);
    }
  }
  /** @brief Invalidate hook. */
  void Invalidate(std::span<std::byte> range) noexcept {
    const bool after_lend = (barriers_ % 2U) == 1U;
    if (!(faults_.skip_invalidate_all || (faults_.skip_invalidate_after && after_lend))) {
      machine_.InvalidateRange(range);
    }
  }
  /** @brief Barrier hook. */
  void Barrier() noexcept {
    ++barriers_;
    machine_.Barrier();
  }

private:
  Machine& machine_;
  const Faults& faults_;
  uint32_t barriers_{0U};
};

/** @brief What `Poll` reports. */
enum class PollState : uint8_t {
  Idle = 0,    ///< Nothing in flight, nothing to report.
  Pending = 1, ///< In flight.
  Done = 2,    ///< Finished; reported once.
};

/** @brief One poll result. */
struct PortPoll {
  PollState state{PollState::Idle};
  kb::Status status{kb::Status::Ok}; ///< Valid when `Done`.
  std::size_t count{0U};             ///< Bytes moved (valid when `Done`).
  uint32_t detail{0U};               ///< The raw hardware flags of an error.
};

/** @brief The tuning of a model port. */
struct PortConfig {
  bool has_dma{true};             ///< False: the board gave this peripheral no stream.
  std::size_t dma_ceiling{1024U}; ///< The largest frame by DMA.
  std::size_t it_ceiling{16U};    ///< The largest frame by interrupt (the fallback).
  std::size_t abort_bound{8U};    ///< Engine ticks an abort waits for EN to read 0.
};

/** @brief The error counters of a model port. */
struct PortStats {
  uint32_t transfer_errors{0U};
  uint32_t fifo_errors{0U};
  uint32_t completions{0U};
  uint32_t timeouts{0U};
  uint32_t cancels{0U};
};

/** @brief The reference port (see the file note). */
template <kb::dma::Region R>
class ModelPort {
public:
  /** @brief A port over `engine` and `ops` with `faults` and `config`. */
  ModelPort(DmaEngine& engine, ModelCacheOps& ops, const Faults& faults,
            const PortConfig& config) noexcept
      : engine_(engine), ops_(ops), faults_(faults), config_(config) {}

  /** @brief The key this port gives to the buffers it uses. */
  [[nodiscard]] kb::dma::PortKey<ModelPort> Key() const noexcept {
    return kb::dma::PortKey<ModelPort>{};
  }

  /** @brief What the port offers now. */
  [[nodiscard]] kb::dma::Capabilities Capabilities() const noexcept {
    kb::dma::Capabilities c{};
    const bool dma = config_.has_dma || faults_.silent_fallback;
    c.mode = dma ? kb::dma::TransferMode::Dma : kb::dma::TransferMode::Interrupt;
    c.zero_copy = config_.has_dma;
    c.confirmed_abort = true;
    c.max_frame_bytes = config_.has_dma ? config_.dma_ceiling : config_.it_ceiling;
    c.align_bytes = R::kAlignBytes;
    return c;
  }

  /** @brief The DMA part of a handler's health record. */
  [[nodiscard]] kb::dma::DmaHealth Health() const noexcept {
    kb::dma::DmaHealth h{};
    h.mode = Capabilities().mode;
    h.faulted = faulted_;
    h.quarantined = quarantined_;
    h.errors = stats_.transfer_errors;
    return h;
  }

  /** @brief The counters. */
  [[nodiscard]] PortStats Stats() const noexcept {
    return stats_;
  }

  /**
   * @brief Lends `buffer` and starts the engine.
   * @return `Ok`; `OutOfRange` (above the port's ceiling); `Busy`; the `Lend` failures.
   */
  template <std::size_t Bytes>
  [[nodiscard]] kb::Status Start(kb::dma::Buffer<R, Bytes>& buffer, kb::dma::Direction dir,
                                 std::size_t used, uint8_t seed,
                                 kb::utils::Deadline deadline) noexcept {
    kb::Status status = kb::Status::Ok;
    if (used > Capabilities().max_frame_bytes) {
      status = kb::Status::OutOfRange;
    } else if (!latch_.Idle()) {
      status = kb::Status::Busy;
    } else {
      status = buffer.Lend(Key(), dir, used, ops_, lease_);
      if (status == kb::Status::Ok) {
        static_cast<void>(latch_.Arm());
        deadline_ = deadline;
        forced_report_ = false;
        result_ = PortPoll{};
        engine_.Start(dir, lease_.DeviceView(Key()).data(), used, seed);
      }
    }
    return status;
  }

  /** @brief One engine cycle, then the interrupt: services the flags the engine raised. */
  void Tick() noexcept {
    engine_.Tick();
    service();
  }

  /** @brief Reports a finished transfer once, ends an overdue one as `Timeout`. */
  [[nodiscard]] PortPoll Poll(kb::Instant now) noexcept {
    PortPoll out{};
    if (latch_.TakeReport() || forced_report_) {
      forced_report_ = false;
      out = result_;
      out.state = PollState::Done;
      if ((out.status == kb::Status::Ok) && deadline_.Expired(now)) {
        out.status = kb::Status::Timeout;
        ++stats_.timeouts;
      }
    } else if (latch_.Pending()) {
      out.state = PollState::Pending;
      if (deadline_.Expired(now)) {
        out = timeout();
      }
    } else {
      out.state = PollState::Idle;
    }
    return out;
  }

  /**
   * @brief Ends the transfer in flight and returns `Ok` only once the memory is the CPU's again.
   * @return `Ok`; `Fault` (the stop was not confirmed: the buffer is quarantined).
   */
  [[nodiscard]] kb::Status Cancel() noexcept {
    kb::Status status = kb::Status::Ok;
    const kb::dma::CancelClaim claim = latch_.TryCancel();
    if (claim == kb::dma::CancelClaim::CancelledPending) {
      status = abortTransfer();
    }
    if (claim != kb::dma::CancelClaim::Nothing) {
      ++stats_.cancels;
      latch_.FinishCancel();
    }
    return status;
  }

  /**
   * @brief Re-initialises the peripheral (hard reset of the engine) and releases the quarantined
   * buffer.
   */
  template <std::size_t Bytes>
  [[nodiscard]] kb::Status Recover(kb::dma::Buffer<R, Bytes>& buffer) noexcept {
    engine_.HardReset();
    const kb::Status status = buffer.Recover(Key(), ops_, kb::dma::StopEvidence::Confirmed);
    if (status == kb::Status::Ok) {
      faulted_ = false;
    }
    return status;
  }

private:
  void service() noexcept {
    const uint32_t flags = engine_.Flags();
    const bool error = (flags & kFlagTransferError) != 0U;
    const bool complete = (flags & kFlagComplete) != 0U;
    uint32_t handled = 0U;
    if ((flags & kFlagFifoError) != 0U) {
      ++stats_.fifo_errors;
      handled |= kFlagFifoError;
    }
    if (error) {
      ++stats_.transfer_errors;
      handled |= kFlagTransferError;
      if (faults_.global_flag_clear) {
        engine_.ClearAllFlags();
        // The FIFO flag was cleared with the rest before this interrupt counted it.
        if ((handled & kFlagFifoError) != 0U) {
          --stats_.fifo_errors;
        }
      }
    }
    if (complete) {
      handled |= kFlagComplete;
    }
    engine_.ClearFlags(handled);
    if (error || complete) {
      finish(error, flags);
    }
  }

  void finish(bool error, uint32_t flags) noexcept {
    if (latch_.TryComplete()) {
      result_.state = PollState::Done;
      result_.count = engine_.Moved();
      result_.detail = error ? flags : 0U;
      result_.status = (error && !faults_.report_ok_on_error) ? kb::Status::Fault : kb::Status::Ok;
      static_cast<void>(
          lease_.Complete(Key(), ops_, error ? kb::dma::Outcome::Error : kb::dma::Outcome::Ok));
      ++stats_.completions;
    } else if (faults_.double_report) {
      result_.state = PollState::Done;
      result_.status = kb::Status::Ok;
      result_.count = engine_.Moved();
      forced_report_ = true;
    } else {
      // The cancel got there first: the completion is dropped.
    }
  }

  [[nodiscard]] PortPoll timeout() noexcept {
    PortPoll out{};
    const kb::dma::CancelClaim claim = latch_.TryCancel();
    if (claim == kb::dma::CancelClaim::CancelledPending) {
      if (faults_.timeout_returns_early) {
        static_cast<void>(lease_.Abort(Key(), ops_, kb::dma::StopEvidence::Confirmed));
      } else {
        static_cast<void>(abortTransfer());
      }
      latch_.FinishCancel();
      ++stats_.timeouts;
      out.state = PollState::Done;
      out.status = kb::Status::Timeout;
      out.count = engine_.Moved();
    } else {
      out.state = PollState::Pending;
    }
    return out;
  }

  /// Stops the engine with a bounded wait for EN to read 0 and settles the lease.
  [[nodiscard]] kb::Status abortTransfer() noexcept {
    engine_.RequestStop();
    bool confirmed = faults_.abort_no_wait;
    for (std::size_t i = 0U; (i < config_.abort_bound) && !confirmed; ++i) {
      confirmed = !engine_.Enabled();
      if (!confirmed) {
        engine_.Tick();
      }
    }
    confirmed = confirmed || !engine_.Enabled();
    const bool release = confirmed || faults_.unconfirmed_returns_cpu;
    const kb::Status status = lease_.Abort(
        Key(), ops_,
        release ? kb::dma::StopEvidence::Confirmed : kb::dma::StopEvidence::NotConfirmed);
    if (status != kb::Status::Ok) {
      faulted_ = true;
      ++quarantined_;
    }
    return status;
  }

  DmaEngine& engine_;
  ModelCacheOps& ops_;
  const Faults& faults_;
  PortConfig config_;
  kb::dma::CompletionLatch latch_{};
  kb::dma::Lease<R> lease_{};
  kb::utils::Deadline deadline_;
  PortPoll result_{};
  PortStats stats_{};
  bool forced_report_{false};
  bool faulted_{false};
  uint32_t quarantined_{0U};
};

} // namespace kb::dma::host

#endif // KB_DMA_HOST_MODEL_PORT_HPP
