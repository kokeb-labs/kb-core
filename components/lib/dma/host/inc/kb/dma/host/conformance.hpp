/**
 * @file conformance.hpp
 * @brief The DMA conformance suite, cases T1 to T18, templated over a rig.
 *
 * Each case returns a `Verdict`: `nullptr` when the rig behaved, otherwise a short text naming the
 * first check that failed. A port's host adapter (its boundary linked against the model) and, with
 * loopback wiring, its target adapter provide the same rig member functions as `HostRig` /
 * `AdcRig`, so one suite serves host and bench.
 *
 * | Case | Defect it catches |
 * | --- | --- |
 * | T1 | transmit without a clean: the device reads stale memory |
 * | T2 | receive without an invalidate (with speculative refills), or an invalidate only before |
 * | T3 | an invalidate after a CPU-copy transfer discards the data just written |
 * | T4 | a receive target that shares a cache line with a neighbour |
 * | T5 | the CPU touching a buffer while a lease is out |
 * | T6 | a DMA write after a timeout returned the buffer |
 * | T7 | a stop that does not wait for the enable bit (the burst in flight finishes later) |
 * | T8 | the receive length rounding writing past an unpadded buffer |
 * | T9 | a held frame overwritten, an overrun neither counted nor flagged |
 * | T10 | a lapped (torn) copy published as clean |
 * | T11 | a second drainer delivering a half twice |
 * | T12 | a stall check that subtracts unsigned stamps from two contexts |
 * | T13 | a pool overflow delivered as a frame |
 * | T14 | an error reported as success, or the other pending flag lost to a global clear |
 * | T15 | a cancel racing a completion reporting twice |
 * | T16 | an unconfirmed stop returning the buffer to the CPU |
 * | T17 | a fallback that is not reported |
 * | T18 | a synchronous transfer that makes the caller's memory a DMA target |
 *
 * Test code, outside the MISRA claim.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_DMA_HOST_CONFORMANCE_HPP
#define KB_DMA_HOST_CONFORMANCE_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <utility>

#include "kb/dma/buffer.hpp"
#include "kb/dma/capabilities.hpp"
#include "kb/dma/host/engine.hpp"
#include "kb/dma/host/machine.hpp"
#include "kb/dma/host/model_port.hpp"
#include "kb/dma/host/stream_rig.hpp"
#include "kb/utils/status.hpp"
#include "kb/utils/time.hpp"

namespace kb::dma::host::conformance {

/// `nullptr`: the rig passed; otherwise the first failed check.
using Verdict = const char*;

namespace detail {

[[nodiscard]] inline bool CapturedIs(std::span<const std::byte> got, std::size_t n,
                                     uint8_t seed) noexcept {
  bool same = got.size() == n;
  for (std::size_t i = 0U; same && (i < n); ++i) {
    same = (got[i] == static_cast<std::byte>(static_cast<uint8_t>(seed + i)));
  }
  return same;
}

} // namespace detail

/** @brief T1: the CPU fills the transmit buffer, the DMA must read what the CPU wrote. */
template <typename Rig>
[[nodiscard]] Verdict T1TxNeedsClean(Rig& rig) {
  constexpr std::size_t kN = 40U;
  rig.CpuFill(rig.TxView(), kN, 0x10U);
  Verdict verdict = nullptr;
  if (rig.StartTx(kN) != kb::Status::Ok) {
    verdict = "the transmit did not start";
  } else if (rig.RunToDone().status != kb::Status::Ok) {
    verdict = "the transmit did not complete";
  } else if (!detail::CapturedIs(rig.Engine().Captured(), kN, 0x10U)) {
    verdict = "the device read stale memory (missing clean)";
  } else if (rig.M().Total() != 0U) {
    verdict = "a coherency violation was logged";
  } else {
    verdict = nullptr;
  }
  return verdict;
}

/** @brief T2: a receive into a buffer the CPU wrote before, read back by the CPU after. */
template <typename Rig>
[[nodiscard]] Verdict T2RxNeedsInvalidate(Rig& rig) {
  constexpr std::size_t kN = 40U;
  rig.CpuFill(rig.RxView(), 100U, 0xEEU);
  Verdict verdict = nullptr;
  if (rig.StartRx(kN, 0x20U) != kb::Status::Ok) {
    verdict = "the receive did not start";
  } else if (rig.RunToDone().status != kb::Status::Ok) {
    verdict = "the receive did not complete";
  } else if (!rig.CpuReadsBack(rig.RxView(), kN, 0x20U)) {
    verdict = "the CPU read stale data (missing invalidate)";
  } else if (rig.M().Total() != 0U) {
    verdict = "a coherency violation was logged";
  } else {
    verdict = nullptr;
  }
  return verdict;
}

/** @brief T3: a CPU-copy transfer does no cache maintenance and loses nothing. */
template <typename Rig>
[[nodiscard]] Verdict T3CpuPathNoMaintenance(Rig& rig) {
  constexpr std::size_t kN = 40U;
  rig.CpuPathCopyIn(rig.TxView(), kN, 0x30U);
  const MaintenanceCounts m = rig.M().Maintenance();
  Verdict verdict = nullptr;
  if ((m.cleans != 0U) || (m.invalidates != 0U)) {
    verdict = "a CPU-path transfer ran cache maintenance";
  } else if (!rig.CpuReadsBack(rig.TxView(), kN, 0x30U)) {
    verdict = "the data written by the CPU was lost";
  } else if (rig.M().Total() != 0U) {
    verdict = "a coherency violation was logged";
  } else {
    verdict = nullptr;
  }
  return verdict;
}

/** @brief T4: the CPU writes a neighbouring object while the DMA receives; it must survive. */
template <typename Rig>
[[nodiscard]] Verdict T4NeighbourSurvives(Rig& rig) {
  constexpr std::size_t kN = 40U;
  Verdict verdict = nullptr;
  if (!rig.BeginReceive(kN, 0x40U)) {
    verdict = "the receive did not start";
  } else {
    rig.FillNeighbour(kN, 0x5AU);
    if (!rig.FinishReceive()) {
      verdict = "the receive did not complete";
    } else if (!rig.TargetReadsBack(kN, 0x40U)) {
      verdict = "the received data is wrong";
    } else if (!rig.NeighbourIntact(kN, 0x5AU)) {
      verdict = "a neighbouring object was destroyed (shared line)";
    } else if ((rig.M().Count(Violation::LostWrite) != 0U) ||
               (rig.M().Count(Violation::UnalignedMaintenance) != 0U)) {
      verdict = "maintenance reached lines the buffer does not own";
    } else {
      verdict = nullptr;
    }
  }
  return verdict;
}

/** @brief T5: while a lease is out the CPU has no view and touches nothing. */
template <typename Rig>
[[nodiscard]] Verdict T5NoCpuDuringLease(Rig& rig) {
  Verdict verdict = nullptr;
  if (rig.StartTx(40U) != kb::Status::Ok) {
    verdict = "the transmit did not start";
  } else if (!rig.TxView().empty()) {
    verdict = "the CPU has a view while the lease is out";
  } else {
    rig.CpuFill(rig.TxView(), 8U, 0x77U);
    static_cast<void>(rig.RunToDone());
    verdict = (rig.M().Count(Violation::AccessDuringLease) != 0U)
                  ? "the CPU accessed a leased buffer"
                  : nullptr;
  }
  return verdict;
}

/** @brief T6: after a timeout the DMA must not write the buffer the port returned. */
template <typename Rig>
[[nodiscard]] Verdict T6NoWriteAfterTimeout(Rig& rig) {
  rig.Engine().Hold(true);
  Verdict verdict = nullptr;
  if (rig.StartRx(60U, 0x50U, rig.DeadlineIn(10U)) != kb::Status::Ok) {
    verdict = "the receive did not start";
  } else {
    rig.Ticks(5U);
    if (rig.Port().Poll(rig.Now()).state != PollState::Pending) {
      verdict = "the transfer did not stay pending before its deadline";
    } else {
      rig.AdvanceUs(20000U);
      const PortPoll poll = rig.Port().Poll(rig.Now());
      rig.Engine().Hold(false);
      rig.Ticks(100U);
      if ((poll.state != PollState::Done) || (poll.status != kb::Status::Timeout)) {
        verdict = "the overdue transfer was not reported as a timeout";
      } else if (rig.Engine().Enabled() || (rig.M().Count(Violation::DmaAfterReturn) != 0U)) {
        verdict = "the DMA wrote after the buffer was returned";
      } else {
        verdict = nullptr;
      }
    }
  }
  return verdict;
}

/** @brief T7: a cancel waits for the enable bit; the burst in flight does not land after it
 * returned. */
template <typename Rig>
[[nodiscard]] Verdict T7AbortWaitsForEnable(Rig& rig) {
  Verdict verdict = nullptr;
  if (rig.StartRx(60U, 0x60U) != kb::Status::Ok) {
    verdict = "the receive did not start";
  } else {
    rig.Ticks(10U);
    const kb::Status cancel = rig.Port().Cancel();
    const std::size_t moved = rig.Engine().Moved();
    rig.Ticks(100U);
    if (cancel != kb::Status::Ok) {
      verdict = "the cancel did not return Ok";
    } else if (rig.Engine().Enabled() || (rig.Engine().Moved() != moved) ||
               (rig.M().Count(Violation::DmaAfterReturn) != 0U)) {
      verdict = "the DMA wrote after the cancel returned";
    } else if (!rig.CpuReadsBack(rig.RxView(), moved, 0x60U)) {
      verdict = "the data received before the cancel is wrong";
    } else {
      verdict = nullptr;
    }
  }
  return verdict;
}

/** @brief T8: the receive length rounding (the engine writes up to 3 bytes more) stays inside the
 * buffer. */
template <typename Rig>
[[nodiscard]] Verdict T8RoundingStaysInside(Rig& rig) {
  constexpr std::size_t kN = 5U;
  rig.FillNeighbour(kN, 0x5AU);
  Verdict verdict = nullptr;
  if (!rig.ReceiveIntoTarget(kN, 0x70U)) {
    verdict = "the receive did not complete";
  } else if (rig.M().Count(Violation::DmaOutsideBuffer) != 0U) {
    verdict = "the DMA wrote past the end of the buffer";
  } else if (!rig.NeighbourIntact(kN, 0x5AU)) {
    verdict = "the bytes behind the buffer were overwritten";
  } else if (!rig.TargetReadsBack(kN, 0x70U)) {
    verdict = "the received data is wrong";
  } else {
    verdict = nullptr;
  }
  return verdict;
}

namespace detail {

/// What `DrainAll` found.
struct Drained {
  std::array<uint32_t, 16> seq{};
  std::array<bool, 16> gap{};
  std::array<bool, 16> torn{};
  std::array<bool, 16> consistent{};
  std::size_t count{0U};
};

template <typename S>
[[nodiscard]] Drained DrainAll(S& stream) {
  Drained d{};
  typename S::FrameView view{};
  while ((d.count < d.seq.size()) && (stream.TryTake(view) == kb::Status::Ok)) {
    d.seq[d.count] = view.Seq();
    d.gap[d.count] = view.Gap();
    d.torn[d.count] = view.Torn();
    d.consistent[d.count] = ScanConsistent(view.Data());
    ++d.count;
    stream.Give(std::move(view));
  }
  return d;
}

} // namespace detail

/** @brief T9: the consumer holds a frame for more than a period; the frame must not change, the
 * overrun must show. */
template <typename Rig>
[[nodiscard]] Verdict T9HeldFrame(Rig& rig) {
  using View = typename std::remove_reference_t<decltype(rig.StreamRef())>::FrameView;
  auto& stream = rig.StreamRef();
  Verdict verdict = nullptr;
  if (rig.Start(kb::Instant{0U}) != kb::Status::Ok) {
    verdict = "the stream did not start";
  } else {
    rig.RunHalves(1U, kb::Instant{1000U});
    View held{};
    if (stream.TryTake(held) != kb::Status::Ok) {
      verdict = "the first frame was not delivered";
    } else {
      const Scan first = held.Data();
      rig.RunHalves(3U, kb::Instant{2000U});
      if (held.Data().v != first.v) {
        verdict = "a frame the consumer holds changed under it";
      } else if (stream.Stats().overruns == 0U) {
        verdict = "the overrun was not counted";
      } else {
        stream.Give(std::move(held));
        const detail::Drained before = detail::DrainAll(stream);
        rig.RunHalves(1U, kb::Instant{3000U});
        const detail::Drained after = detail::DrainAll(stream);
        bool gap_seen = false;
        for (std::size_t i = 0U; i < after.count; ++i) {
          gap_seen = gap_seen || after.gap[i];
        }
        if (!gap_seen) {
          verdict = "the frame after the lost ones is not flagged as a gap";
        } else if ((before.count != 0U) && (after.count != 0U) &&
                   (after.seq[0] <= before.seq[before.count - 1U])) {
          verdict = "a frame was delivered twice or out of order";
        } else {
          verdict = nullptr;
        }
      }
    }
  }
  return verdict;
}

/** @brief T10: a late interrupt finds both flags pending; the overwritten half must not be
 * published as clean. */
template <typename Rig>
[[nodiscard]] Verdict T10LappedIsFlagged(Rig& rig) {
  auto& stream = rig.StreamRef();
  Verdict verdict = nullptr;
  if (rig.Start(kb::Instant{0U}) != kb::Status::Ok) {
    verdict = "the stream did not start";
  } else {
    rig.Advance((2U * HwRing::kSamples) + 2U);
    rig.Isr(kb::Instant{1000U});
    const detail::Drained d = detail::DrainAll(stream);
    bool mixed_unflagged = false;
    bool any_flagged = false;
    for (std::size_t i = 0U; i < d.count; ++i) {
      mixed_unflagged = mixed_unflagged || (!d.consistent[i] && !d.torn[i]);
      any_flagged = any_flagged || d.torn[i];
    }
    if (mixed_unflagged) {
      verdict = "a torn frame was published as clean";
    } else if (!any_flagged) {
      verdict = "the lapped half was not flagged";
    } else {
      verdict = nullptr;
    }
  }
  return verdict;
}

/** @brief T11: the interrupt and a task both drain the flags; each half must be delivered once. */
template <typename Rig>
[[nodiscard]] Verdict T11SingleDrainer(Rig& rig) {
  auto& stream = rig.StreamRef();
  Verdict verdict = nullptr;
  if (rig.Start(kb::Instant{0U}) != kb::Status::Ok) {
    verdict = "the stream did not start";
  } else {
    rig.Advance(HwRing::kSamples);
    rig.TaskDrainPreempted(kb::Instant{500U});
    rig.Isr(kb::Instant{500U});
    const detail::Drained d = detail::DrainAll(stream);
    if (stream.Stats().published != rig.HalvesDone()) {
      verdict = "a half was delivered more than once";
    } else if (d.count != 1U) {
      verdict = "the consumer saw a half twice";
    } else {
      verdict = nullptr;
    }
  }
  return verdict;
}

/** @brief T12: a frame stamped by the interrupt after the task read its clock is not a stall. */
template <typename Rig>
[[nodiscard]] Verdict T12SignedStall(Rig& rig) {
  auto& stream = rig.StreamRef();
  Verdict verdict = nullptr;
  if (rig.Start(kb::Instant{0U}) != kb::Status::Ok) {
    verdict = "the stream did not start";
  } else {
    const kb::Instant task_now{1000U};
    rig.RunHalves(1U, kb::Instant{1050U});
    if (stream.CheckStall(task_now)) {
      verdict = "a healthy stream was reported stalled (unsigned elapsed time)";
    } else if (!stream.CheckStall(kb::Instant{1050U + 10000U})) {
      verdict = "a stalled stream was not detected";
    } else {
      verdict = nullptr;
    }
  }
  return verdict;
}

/** @brief T13: a pool overflow is counted and never delivered as a frame. */
template <typename Rig>
[[nodiscard]] Verdict T13OverflowIsNotData(Rig& rig) {
  using View = typename std::remove_reference_t<decltype(rig.StreamRef())>::FrameView;
  auto& stream = rig.StreamRef();
  Verdict verdict = nullptr;
  if (rig.Start(kb::Instant{0U}) != kb::Status::Ok) {
    verdict = "the stream did not start";
  } else {
    rig.RaiseOverrun();
    rig.Isr(kb::Instant{100U});
    View view{};
    if (stream.TryTake(view) != kb::Status::Empty) {
      verdict = "an overflow was delivered as a frame";
    } else if (stream.Stats().hw_overruns != 1U) {
      verdict = "the overflow was not counted";
    } else {
      rig.RunHalves(1U, kb::Instant{200U});
      verdict = ((stream.TryTake(view) == kb::Status::Ok) && view.Gap())
                    ? nullptr
                    : "the frame after an overflow is not flagged";
    }
  }
  return verdict;
}

/** @brief T14: a transfer error and a FIFO error mid-transfer; one completion, flags cleared one by
 * one. */
template <typename Rig>
[[nodiscard]] Verdict T14ErrorFlags(Rig& rig) {
  Verdict verdict = nullptr;
  if (rig.StartRx(60U, 0x20U) != kb::Status::Ok) {
    verdict = "the receive did not start";
  } else {
    rig.Engine().InjectError(20U, kFlagTransferError | kFlagFifoError);
    const PortPoll poll = rig.RunToDone();
    const PortPoll again = rig.Port().Poll(rig.Now());
    const PortStats stats = rig.Port().Stats();
    if ((poll.state != PollState::Done) || (poll.status != kb::Status::Fault) ||
        ((poll.detail & kFlagTransferError) == 0U)) {
      verdict = "the error was not reported as a fault with its flags";
    } else if (again.state != PollState::Idle) {
      verdict = "the completion was reported twice";
    } else if ((stats.transfer_errors != 1U) || (stats.fifo_errors != 1U)) {
      verdict = "an error flag was lost (global clear)";
    } else if ((rig.Engine().Flags() != 0U) ||
               (rig.RxStage().Current() != kb::dma::BufferState::Cpu)) {
      verdict = "the flags or the buffer were not settled";
    } else {
      verdict = nullptr;
    }
  }
  return verdict;
}

/** @brief T15: a cancel racing a completion yields exactly one outcome and the memory back. */
template <typename Rig>
[[nodiscard]] Verdict T15CancelRacesCompletion(Rig& rig) {
  Verdict verdict = nullptr;
  // Completion first: the interrupt has run, nobody polled, then the cancel.
  if (rig.StartRx(8U, 0x10U) != kb::Status::Ok) {
    verdict = "the receive did not start";
  } else {
    rig.Ticks(20U);
    const kb::Status cancel = rig.Port().Cancel();
    const PortPoll after = rig.Port().Poll(rig.Now());
    if ((cancel != kb::Status::Ok) || (after.state != PollState::Idle)) {
      verdict = "a completion dropped by the cancel was reported";
    } else if (rig.RxStage().Current() != kb::dma::BufferState::Cpu) {
      verdict = "the buffer is not the CPU's after the cancel";
    } else {
      // Cancel first: the engine finished but the interrupt has not run when the cancel claims the
      // transfer; the interrupt then arrives late.
      if (rig.StartRx(8U, 0x10U) != kb::Status::Ok) {
        verdict = "the second receive did not start";
      } else {
        rig.Engine().Tick();
        rig.Engine().Tick();
        rig.Engine().Tick();
        rig.Engine().Tick();
        rig.Engine().Tick();
        rig.Engine().Tick();
        rig.Engine().Tick();
        rig.Engine().Tick();
        const kb::Status second = rig.Port().Cancel();
        rig.Port().Tick();
        const PortPoll late = rig.Port().Poll(rig.Now());
        if (second != kb::Status::Ok) {
          verdict = "the cancel did not return Ok";
        } else if (late.state != PollState::Idle) {
          verdict = "a completion that lost to the cancel was reported";
        } else {
          verdict = nullptr;
        }
      }
    }
  }
  return verdict;
}

/** @brief T16: a stop that is never confirmed quarantines the buffer; only a recovery returns it.
 * The rig's engine is built stuck. */
template <typename Rig>
[[nodiscard]] Verdict T16UnconfirmedQuarantines(Rig& rig) {
  Verdict verdict = nullptr;
  if (rig.StartRx(100U, 0x30U) != kb::Status::Ok) {
    verdict = "the receive did not start";
  } else {
    rig.Ticks(5U);
    const kb::Status cancel = rig.Port().Cancel();
    if (cancel != kb::Status::Fault) {
      verdict = "an unconfirmed stop did not return Fault";
    } else if ((rig.RxStage().Current() != kb::dma::BufferState::Quarantined) ||
               !rig.RxView().empty()) {
      verdict = "the buffer is not quarantined";
    } else if (!rig.Port().Health().faulted) {
      verdict = "the health record does not show the fault";
    } else if (rig.StartRx(8U, 0x30U) == kb::Status::Ok) {
      verdict = "a quarantined buffer was lent again";
    } else if (rig.Port().Recover(rig.RxStage()) != kb::Status::Ok) {
      verdict = "the recovery did not return the buffer";
    } else {
      rig.Ticks(100U);
      verdict = ((rig.RxStage().Current() == kb::dma::BufferState::Cpu) &&
                 (rig.M().Count(Violation::DmaAfterReturn) == 0U) && !rig.Port().Health().faulted)
                    ? nullptr
                    : "the buffer was not settled after the recovery";
    }
  }
  return verdict;
}

/** @brief T17: a port without a DMA stream says so, and its ceiling applies. */
template <typename Rig>
[[nodiscard]] Verdict T17FallbackIsReported(Rig& rig, std::size_t expected_ceiling) {
  const kb::dma::Capabilities caps = rig.Port().Capabilities();
  Verdict verdict = nullptr;
  if (caps.mode == kb::dma::TransferMode::Dma) {
    verdict = "the port reports DMA while serving by interrupt";
  } else if (caps.max_frame_bytes != expected_ceiling) {
    verdict = "the published ceiling is not the interrupt ceiling";
  } else if (rig.Port().Health().mode != caps.mode) {
    verdict = "the health record does not show the mode";
  } else if (rig.StartRx(expected_ceiling + 1U, 0x10U) != kb::Status::OutOfRange) {
    verdict = "a frame above the ceiling was accepted";
  } else {
    verdict = nullptr;
  }
  return verdict;
}

/** @brief T18: a synchronous transfer with an unaligned caller buffer works and writes nothing past
 * it. */
template <typename Rig>
[[nodiscard]] Verdict T18SyncThroughStaging(Rig& rig) {
  rig.ArmCallerCanaries();
  const std::array<std::byte, 7> tx{std::byte{1U}, std::byte{2U}, std::byte{3U}, std::byte{4U},
                                    std::byte{5U}, std::byte{6U}, std::byte{7U}};
  std::span<std::byte> caller = rig.CallerBuffer();
  Verdict verdict = nullptr;
  if (rig.SyncTransfer(tx, caller) != kb::Status::Ok) {
    verdict = "the transfer failed";
  } else if (!rig.CpuReadsBack(caller, caller.size(), Rig::kRxSeed)) {
    verdict = "the caller received the wrong data";
  } else if (!rig.CallerCanariesIntact() || (rig.M().Count(Violation::DmaOutsideBuffer) != 0U)) {
    verdict = "the DMA wrote beyond the caller's buffer";
  } else if (rig.M().Count(Violation::UnalignedMaintenance) != 0U) {
    verdict = "maintenance reached the caller's lines";
  } else {
    verdict = nullptr;
  }
  return verdict;
}

} // namespace kb::dma::host::conformance

#endif // KB_DMA_HOST_CONFORMANCE_HPP
