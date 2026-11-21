/**
 * @file engine.hpp
 * @brief A one-channel DMA engine for the host model: it moves bytes between the machine's memory
 * and a scripted peripheral, a few bytes per `Tick()`, and behaves like the hardware the
 * conformance suite guards against.
 *
 * - The enable bit (`Enabled()`) clears only after a stop request **and** the current burst has
 *   finished (the stream may complete the burst in flight after EN is cleared), or at once on a
 *   transfer error; `Stuck` makes the engine ignore stop requests until its job ends.
 * - A receive job may round its length up (`round_rx_to`), writing junk bytes past the requested
 *   length (the SPI receive rounding of some parts).
 * - Flags are a register: `Flags()` reads, `ClearFlags(mask)` clears individual bits,
 *   `ClearAllFlags()` clears everything (the global clear).
 * - `Hold(true)` freezes progress (a stalled peripheral).
 *
 * Receive data is `seed + index` (bytes past the request are 0xEE); transmit data read from memory
 * is captured in `Captured()`.
 *
 * Test code, outside the MISRA claim.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_DMA_HOST_ENGINE_HPP
#define KB_DMA_HOST_ENGINE_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "kb/dma/host/machine.hpp"
#include "kb/dma/region.hpp"

namespace kb::dma::host {

/// Transfer complete.
inline constexpr uint32_t kFlagComplete = 1U;
/// Transfer error: the engine disabled itself.
inline constexpr uint32_t kFlagTransferError = 2U;
/// FIFO error: counted, the engine keeps running.
inline constexpr uint32_t kFlagFifoError = 4U;

/** @brief The engine's behaviour knobs. */
struct EngineConfig {
  std::size_t burst_bytes{4U};    ///< A stop waits for the burst in flight to end.
  std::size_t bytes_per_tick{1U}; ///< Progress per `Tick()`.
  std::size_t round_rx_to{1U};    ///< A receive job's length is rounded up to a multiple of this.
  bool stuck{false};              ///< Ignores stop requests until the job ends.
};

/** @brief The one-channel DMA engine (see the file note). */
class DmaEngine {
public:
  /// The most transmit bytes captured.
  static constexpr std::size_t kCaptureBytes = 1536U;

  /** @brief An idle engine on `machine`. */
  explicit DmaEngine(Machine& machine, const EngineConfig& config = EngineConfig{}) noexcept
      : machine_(machine), config_(config) {}

  /** @brief Replaces the behaviour knobs (between jobs). */
  void Configure(const EngineConfig& config) noexcept {
    config_ = config;
  }

  /**
   * @brief Starts a job and sets EN.
   * @param dir    `FromDevice`: the engine writes `seed + i` into memory; `ToDevice`: it reads
   * memory.
   * @param memory The first byte of the memory window.
   * @param length The requested length in bytes.
   * @param seed   The receive pattern's first byte.
   */
  void Start(kb::dma::Direction dir, std::byte* memory, std::size_t length, uint8_t seed) noexcept {
    dir_ = dir;
    mem_ = memory;
    length_ = length;
    total_ = (dir == kb::dma::Direction::FromDevice) ? kb::dma::RoundUp(length, config_.round_rx_to)
                                                     : length;
    seed_ = seed;
    done_ = 0U;
    captured_count_ = 0U;
    stop_requested_ = false;
    burst_left_ = 0U;
    en_ = true;
  }

  /** @brief The enable bit: true until the job ends, an error disables it, or a stop completes. */
  [[nodiscard]] bool Enabled() const noexcept {
    return en_;
  }

  /** @brief Bytes moved so far (including rounding bytes). */
  [[nodiscard]] std::size_t Moved() const noexcept {
    return done_;
  }

  /** @brief The flag register. */
  [[nodiscard]] uint32_t Flags() const noexcept {
    return flags_;
  }
  /** @brief Clears the flags in `mask` only. */
  void ClearFlags(uint32_t mask) noexcept {
    flags_ &= ~mask;
  }
  /** @brief Clears every flag (a global clear). */
  void ClearAllFlags() noexcept {
    flags_ = 0U;
  }

  /** @brief Freezes (true) or releases (false) progress. */
  void Hold(bool on) noexcept {
    held_ = on;
  }

  /** @brief Raises `mask` when `after_bytes` have moved; `kFlagTransferError` also disables the
   * engine. */
  void InjectError(std::size_t after_bytes, uint32_t mask) noexcept {
    inject_at_ = after_bytes;
    inject_mask_ = mask;
    inject_armed_ = true;
  }

  /** @brief Clears EN as a request: the engine finishes its burst first. */
  void RequestStop() noexcept {
    if (en_ && !stop_requested_) {
      stop_requested_ = true;
      const std::size_t burst = (config_.burst_bytes == 0U) ? 1U : config_.burst_bytes;
      burst_left_ = (burst - (done_ % burst)) % burst;
    }
  }

  /** @brief A peripheral reset: the engine stops at once, flags clear, nothing is held. */
  void HardReset() noexcept {
    en_ = false;
    stop_requested_ = false;
    held_ = false;
    flags_ = 0U;
    inject_armed_ = false;
    config_.stuck = false;
  }

  /** @brief The bytes a transmit job read from memory so far. */
  [[nodiscard]] std::span<const std::byte> Captured() const noexcept {
    return {captured_.data(), captured_count_};
  }

  /**
   * @brief One engine cycle: moves up to `bytes_per_tick` bytes (unless held), or finishes a stop.
   * A stop completes when the burst in flight has ended, even while the peripheral is held.
   */
  void Tick() noexcept {
    if (en_) {
      const bool stopping = stop_requested_ && !config_.stuck;
      if (stopping && (burst_left_ == 0U)) {
        en_ = false;
      } else if (!held_) {
        if (dir_ == kb::dma::Direction::FromDevice) {
          machine_.Speculate();
        }
        for (std::size_t i = 0U; (i < config_.bytes_per_tick) && en_ && (done_ < total_); ++i) {
          moveOne();
          if (stopping && (burst_left_ > 0U)) {
            --burst_left_;
          }
        }
        finishIfDone();
      } else {
        // Held: no progress, no stop yet.
      }
    }
  }

private:
  void moveOne() noexcept {
    if (dir_ == kb::dma::Direction::FromDevice) {
      const std::byte value = (done_ < length_)
                                  ? static_cast<std::byte>(static_cast<uint8_t>(seed_ + done_))
                                  : std::byte{0xEEU};
      machine_.DmaWrite(mem_ + done_, std::span<const std::byte>(&value, 1U));
    } else {
      std::byte value{0U};
      machine_.DmaRead(mem_ + done_, std::span<std::byte>(&value, 1U));
      if (captured_count_ < captured_.size()) {
        captured_[captured_count_] = value;
        ++captured_count_;
      }
    }
    ++done_;
    if (inject_armed_ && (done_ >= inject_at_)) {
      inject_armed_ = false;
      flags_ |= inject_mask_;
      if ((inject_mask_ & kFlagTransferError) != 0U) {
        en_ = false;
      }
    }
  }

  void finishIfDone() noexcept {
    if (en_ && (done_ >= total_)) {
      en_ = false;
      flags_ |= kFlagComplete;
    }
  }

  Machine& machine_;
  EngineConfig config_;
  kb::dma::Direction dir_{kb::dma::Direction::ToDevice};
  std::byte* mem_{nullptr};
  std::size_t length_{0U};
  std::size_t total_{0U};
  uint8_t seed_{0U};
  std::size_t done_{0U};
  bool en_{false};
  bool held_{false};
  bool stop_requested_{false};
  std::size_t burst_left_{0U};
  uint32_t flags_{0U};
  bool inject_armed_{false};
  std::size_t inject_at_{0U};
  uint32_t inject_mask_{0U};
  std::array<std::byte, kCaptureBytes> captured_{};
  std::size_t captured_count_{0U};
};

} // namespace kb::dma::host

#endif // KB_DMA_HOST_ENGINE_HPP
