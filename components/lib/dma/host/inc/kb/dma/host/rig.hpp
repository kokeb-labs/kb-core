/**
 * @file rig.hpp
 * @brief The test rig of the conformance suite for buffer transfers: a machine, an engine, a model
 * port and the buffers, memory arena and helpers the cases T1 to T8 and T14 to T18 use.
 *
 * A real port supplies the same member functions over its boundary (a hardware rig with loopback
 * wiring); the cases in `conformance.hpp` are templates over the rig.
 *
 * Test code, outside the MISRA claim.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_DMA_HOST_RIG_HPP
#define KB_DMA_HOST_RIG_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "kb/dma/buffer.hpp"
#include "kb/dma/host/engine.hpp"
#include "kb/dma/host/machine.hpp"
#include "kb/dma/host/model_port.hpp"
#include "kb/dma/region.hpp"
#include "kb/utils/time.hpp"

namespace kb::dma::host {

/// The master number of the model's DMA engine.
inline constexpr kb::dma::MasterSet kModelDma = kb::dma::MasterBit(0U);

/// A cached region with 32-byte lines (a Cortex-M7 D-cache): buffers are cleaned and invalidated.
struct ModelMaintained {
  static constexpr kb::dma::CachePolicy kPolicy = kb::dma::CachePolicy::Maintained;
  static constexpr std::size_t kLineBytes = 32U;
  static constexpr std::size_t kAlignBytes = 32U;
  static constexpr kb::dma::MasterSet kMasters = kModelDma;
};

/// An uncached region (an MPU non-cacheable window): no maintenance.
struct ModelUncached {
  static constexpr kb::dma::CachePolicy kPolicy = kb::dma::CachePolicy::NonCacheable;
  static constexpr std::size_t kLineBytes = 1U;
  static constexpr std::size_t kAlignBytes = 4U;
  static constexpr kb::dma::MasterSet kMasters = kModelDma;
};

/** @brief How a rig is built. */
struct RigOptions {
  bool speculation{true}; ///< The cache refills lines speculatively during a transfer.
  EngineConfig engine{};  ///< The engine's behaviour.
  PortConfig port{};      ///< The port's tuning.
};

/** @brief The rig (see the file note). Not copyable or movable: its parts refer to each other. */
// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding)
class HostRig {
public:
  /// The region of the staging buffers.
  using Region = ModelMaintained;
  /// The usable size of a staging buffer (padded to 128 by the region).
  static constexpr std::size_t kStageBytes = 100U;
  /// A staging buffer.
  using Stage = kb::dma::Buffer<Region, kStageBytes>;
  /// The bytes of the neighbour object.
  static constexpr std::size_t kNeighbourBytes = 32U;
  /// The offset of a caller buffer in the arena.
  static constexpr std::size_t kCallerOffset = 259U;

  /** @brief A rig with the defects in `faults`. */
  explicit HostRig(const Faults& faults = Faults{}, const RigOptions& options = RigOptions{})
      : faults_(faults), machine_(Region::kLineBytes, options.speculation),
        engine_(machine_, options.engine), ops_(machine_, faults_),
        port_(engine_, ops_, faults_, options.port) {
    const auto key = port_.Key();
    static_cast<void>(machine_.RegisterBuffer(tx_.RawMemory(key), tx_.Header(), true));
    static_cast<void>(
        machine_.RegisterBuffer(pack_.stage.RawMemory(key), pack_.stage.Header(), true));
    static_cast<void>(machine_.RegisterRaw(pack_.neighbour, true));
    static_cast<void>(machine_.RegisterRaw(arena_.mem, true));
  }
  HostRig(const HostRig&) = delete;
  HostRig& operator=(const HostRig&) = delete;
  HostRig(HostRig&&) = delete;
  HostRig& operator=(HostRig&&) = delete;
  ~HostRig() = default;

  /** @name Parts
   * @{ */
  [[nodiscard]] Machine& M() noexcept {
    return machine_;
  }
  [[nodiscard]] DmaEngine& Engine() noexcept {
    return engine_;
  }
  [[nodiscard]] ModelPort<Region>& Port() noexcept {
    return port_;
  }
  [[nodiscard]] ModelCacheOps& Ops() noexcept {
    return ops_;
  }
  [[nodiscard]] Stage& TxStage() noexcept {
    return tx_;
  }
  [[nodiscard]] Stage& RxStage() noexcept {
    return pack_.stage;
  }
  /** @} */

  /** @brief The CPU's view of the transmit staging buffer (a defective rig hands out the raw
   * memory). */
  [[nodiscard]] std::span<std::byte> TxView() noexcept {
    return faults_.view_during_lease ? tx_.RawMemory(port_.Key()).first(kStageBytes)
                                     : tx_.CpuView();
  }
  /** @brief The CPU's view of the receive staging buffer. */
  [[nodiscard]] std::span<std::byte> RxView() noexcept {
    return faults_.view_during_lease ? pack_.stage.RawMemory(port_.Key()).first(kStageBytes)
                                     : pack_.stage.CpuView();
  }

  /** @brief The CPU stores `seed + i` for `i < n` into `view` through the cache. */
  void CpuFill(std::span<std::byte> view, std::size_t n, uint8_t seed) noexcept {
    for (std::size_t i = 0U; (i < n) && (i < view.size()); ++i) {
      const auto b = static_cast<std::byte>(static_cast<uint8_t>(seed + i));
      machine_.CpuStore(&view[i], std::span<const std::byte>(&b, 1U));
    }
  }

  /** @brief True when `view[0..n)` reads back as `seed + i` through the cache (false for an empty
   * view). */
  [[nodiscard]] bool CpuReadsBack(std::span<std::byte> view, std::size_t n, uint8_t seed) noexcept {
    bool same = view.size() >= n;
    for (std::size_t i = 0U; same && (i < n); ++i) {
      std::byte b{0U};
      machine_.CpuLoad(&view[i], std::span<std::byte>(&b, 1U));
      same = (b == static_cast<std::byte>(static_cast<uint8_t>(seed + i)));
    }
    return same;
  }

  /** @brief Starts a transmit of `n` bytes of the transmit staging buffer. */
  [[nodiscard]] kb::Status StartTx(std::size_t n) noexcept {
    return port_.Start(tx_, kb::dma::Direction::ToDevice, n, 0U, farDeadline());
  }
  /** @brief Starts a receive of `n` bytes into the receive staging buffer. */
  [[nodiscard]] kb::Status StartRx(std::size_t n, uint8_t seed,
                                   kb::utils::Deadline deadline = kb::utils::Deadline{}) noexcept {
    return port_.Start(pack_.stage, kb::dma::Direction::FromDevice, n, seed,
                       deadline.Expired(now_) ? farDeadline() : deadline);
  }

  /** @brief The rig clock. */
  [[nodiscard]] kb::Instant Now() const noexcept {
    return now_;
  }
  /** @brief Advances the rig clock by `us` microseconds. */
  void AdvanceUs(uint64_t us) noexcept {
    now_ = kb::Instant{now_.Us() + us};
  }
  /** @brief A deadline `ms` milliseconds from the rig clock. */
  [[nodiscard]] kb::utils::Deadline DeadlineIn(uint32_t ms) const noexcept {
    return kb::utils::Deadline::After(now_, kb::Milliseconds{ms});
  }

  /** @brief Runs port ticks until the transfer reports or `max_ticks` pass. */
  [[nodiscard]] PortPoll RunToDone(std::size_t max_ticks = 4096U) noexcept {
    PortPoll poll{};
    poll.state = PollState::Pending;
    for (std::size_t i = 0U; (i < max_ticks) && (poll.state == PollState::Pending); ++i) {
      port_.Tick();
      poll = port_.Poll(now_);
    }
    return poll;
  }

  /** @brief Runs `n` port ticks without polling. */
  void Ticks(std::size_t n) noexcept {
    for (std::size_t i = 0U; i < n; ++i) {
      port_.Tick();
    }
  }

  /** @brief T3: the CPU-path transfer: copies `n` bytes into `view` and does what a port does
   * after. */
  void CpuPathCopyIn(std::span<std::byte> view, std::size_t n, uint8_t seed) noexcept {
    CpuFill(view, n, seed);
    if (faults_.cpu_path_invalidates && !view.empty()) {
      ops_.Invalidate(
          std::span<std::byte>(view.data(), kb::dma::RoundUp(view.size(), Region::kLineBytes)));
    }
  }

  /** @brief The neighbour object: 32 bytes the CPU owns, next to the receive target. */
  [[nodiscard]] std::byte* Neighbour(std::size_t n) noexcept {
    return rawReceive() ? (arena_.mem.data() + n) : pack_.neighbour.data();
  }

  /** @brief Fills the neighbour with `value` through the cache (it stays dirty). */
  void FillNeighbour(std::size_t n, uint8_t value) noexcept {
    std::array<std::byte, kNeighbourBytes> bytes{};
    bytes.fill(static_cast<std::byte>(value));
    machine_.CpuStore(Neighbour(n), bytes);
  }

  /** @brief True when the neighbour still reads `value` through the cache. */
  [[nodiscard]] bool NeighbourIntact(std::size_t n, uint8_t value) noexcept {
    std::array<std::byte, kNeighbourBytes> bytes{};
    machine_.CpuLoad(Neighbour(n), bytes);
    bool same = true;
    for (const std::byte b : bytes) {
      same = same && (b == static_cast<std::byte>(value));
    }
    return same;
  }

  /**
   * @brief T4/T8: starts receiving `n` bytes into the receive target. A correct rig uses the
   * receive staging buffer; a rig with `shared_lines` or `unpadded_staging` uses a raw span of
   * exactly `n` bytes at the start of the arena and maintains exactly those bytes, as a port that
   * skips the buffer type would.
   */
  [[nodiscard]] bool BeginReceive(std::size_t n, uint8_t seed) noexcept {
    bool ok = false;
    if (rawReceive()) {
      std::byte* const target = arena_.mem.data();
      static_cast<void>(machine_.OpenWindow(target, n));
      ops_.Clean(std::span<const std::byte>(target, n));
      ops_.Invalidate(std::span<std::byte>(target, n));
      ops_.Barrier();
      engine_.Start(kb::dma::Direction::FromDevice, target, n, seed);
      raw_length_ = n;
      ok = true;
    } else {
      ok = (StartRx(n, seed) == kb::Status::Ok);
    }
    return ok;
  }

  /** @brief Completes the receive started by `BeginReceive`; true when it finished with `Ok`. */
  [[nodiscard]] bool FinishReceive() noexcept {
    bool ok = false;
    if (rawReceive()) {
      std::byte* const target = arena_.mem.data();
      for (std::size_t i = 0U; (i < 4096U) && engine_.Enabled(); ++i) {
        engine_.Tick();
      }
      engine_.ClearAllFlags();
      ops_.Invalidate(std::span<std::byte>(target, raw_length_));
      ops_.Barrier();
      machine_.ClearWindows();
      ok = true;
    } else {
      ok = (RunToDone().status == kb::Status::Ok);
    }
    return ok;
  }

  /** @brief `BeginReceive` and `FinishReceive` back to back. */
  [[nodiscard]] bool ReceiveIntoTarget(std::size_t n, uint8_t seed) noexcept {
    return BeginReceive(n, seed) && FinishReceive();
  }

  /** @brief True when the receive target reads back `seed + i` for `i < n`. */
  [[nodiscard]] bool TargetReadsBack(std::size_t n, uint8_t seed) noexcept {
    return rawReceive() ? CpuReadsBack(std::span<std::byte>(arena_.mem.data(), n), n, seed)
                        : CpuReadsBack(pack_.stage.CpuView(), n, seed);
  }

  /** @brief The caller's buffer for T18: 5 bytes at an odd arena offset with canaries around it. */
  [[nodiscard]] std::span<std::byte> CallerBuffer() noexcept {
    return {arena_.mem.data() + kCallerOffset, kCallerBytes};
  }

  /** @brief Fills the canary bytes around the caller's buffer. */
  void ArmCallerCanaries() noexcept {
    std::array<std::byte, kCanaryBytes> pattern{};
    pattern.fill(std::byte{0xC3U});
    machine_.CpuStore(arena_.mem.data() + kCallerOffset - kCanaryBytes, pattern);
    machine_.CpuStore(arena_.mem.data() + kCallerOffset + kCallerBytes, pattern);
    machine_.EvictAll();
  }

  /** @brief True when the canary bytes around the caller's buffer are untouched. */
  [[nodiscard]] bool CallerCanariesIntact() noexcept {
    std::array<std::byte, kCanaryBytes> before{};
    std::array<std::byte, kCanaryBytes> after{};
    machine_.CpuLoad(arena_.mem.data() + kCallerOffset - kCanaryBytes, before);
    machine_.CpuLoad(arena_.mem.data() + kCallerOffset + kCallerBytes, after);
    bool intact = true;
    for (std::size_t i = 0U; i < kCanaryBytes; ++i) {
      intact = intact && (before[i] == std::byte{0xC3U}) && (after[i] == std::byte{0xC3U});
    }
    return intact;
  }

  /**
   * @brief T18: a synchronous transfer of the caller's `tx` bytes out and `rx.size()` bytes in, as
   * a DMA port serves `Transfer(tx, rx)`: through the staging buffers, the caller's memory never a
   * DMA target. A rig with `dma_into_caller` targets the caller's `rx` directly.
   * @return `Ok` when both phases finished.
   */
  [[nodiscard]] kb::Status SyncTransfer(std::span<const std::byte> tx,
                                        std::span<std::byte> rx) noexcept {
    kb::Status status = kb::Status::Ok;
    if (!tx.empty()) {
      machine_.CpuStore(tx_.CpuView().data(), tx);
      status = StartTx(tx.size());
      if (status == kb::Status::Ok) {
        status = RunToDone().status;
      }
    }
    if ((status == kb::Status::Ok) && !rx.empty()) {
      if (faults_.dma_into_caller) {
        static_cast<void>(machine_.OpenWindow(rx.data(), rx.size()));
        engine_.Start(kb::dma::Direction::FromDevice, rx.data(), rx.size(), kRxSeed);
        for (std::size_t i = 0U; (i < 4096U) && engine_.Enabled(); ++i) {
          engine_.Tick();
        }
        engine_.ClearAllFlags();
        machine_.ClearWindows();
      } else {
        status = StartRx(rx.size(), kRxSeed);
        if (status == kb::Status::Ok) {
          status = RunToDone().status;
        }
        if (status == kb::Status::Ok) {
          std::array<std::byte, kStageBytes> staged{};
          machine_.CpuLoad(pack_.stage.CpuView().data(),
                           std::span<std::byte>(staged.data(), rx.size()));
          machine_.CpuStore(rx.data(), std::span<const std::byte>(staged.data(), rx.size()));
        }
      }
    }
    return status;
  }

  /// The first byte a synchronous receive returns.
  static constexpr uint8_t kRxSeed = 0x60U;
  /// The size of the caller's buffer in T18.
  static constexpr std::size_t kCallerBytes = 5U;
  /// The canary bytes on each side of the caller's buffer.
  static constexpr std::size_t kCanaryBytes = 16U;

private:
  struct alignas(128) Arena {
    std::array<std::byte, 512> mem{};
  };
  struct Pack {
    Stage stage;
    alignas(32) std::array<std::byte, kNeighbourBytes> neighbour{};
  };

  [[nodiscard]] bool rawReceive() const noexcept {
    return faults_.shared_lines || faults_.unpadded_staging;
  }
  [[nodiscard]] kb::utils::Deadline farDeadline() const noexcept {
    return kb::utils::Deadline::After(now_, kb::Milliseconds{1000000U});
  }

  Faults faults_;
  Machine machine_;
  DmaEngine engine_;
  ModelCacheOps ops_;
  ModelPort<Region> port_;
  Stage tx_;
  Pack pack_{};
  Arena arena_{};
  kb::Instant now_;
  std::size_t raw_length_{0U};
};

} // namespace kb::dma::host

#endif // KB_DMA_HOST_RIG_HPP
