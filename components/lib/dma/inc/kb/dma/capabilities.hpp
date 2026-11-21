/**
 * @file capabilities.hpp
 * @brief What a port object can do right now, and the `mode` a handler puts in its health record.
 *
 * A port tries, per peripheral and decided at build time from the board: DMA as declared, then
 * interrupt-driven with its published ceiling, then polled. The choice is never silent: a port
 * object answers `Capabilities()`, the image report lists each bus's mode, and a handler's health
 * record carries a `DmaHealth` whose `mode` says which path is serving.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_DMA_CAPABILITIES_HPP
#define KB_DMA_CAPABILITIES_HPP

#include <concepts>
#include <cstddef>
#include <cstdint>

namespace kb::dma {

/** @brief Which path moves the data of a peripheral. */
enum class TransferMode : uint8_t {
  Dma = 0,       ///< DMA, as the board declared.
  Interrupt = 1, ///< Interrupt-driven, up to the port's published ceiling.
  Polled = 2,    ///< Polled and bounded by the frame's wire time plus the device timeout.
};

/** @brief The name of a mode for logs: "dma", "interrupt", "polled" ("unknown" outside the range).
 */
[[nodiscard]] constexpr const char* ToString(TransferMode mode) noexcept {
  const char* name = nullptr;
  switch (mode) {
  case TransferMode::Dma:
    name = "dma";
    break;
  case TransferMode::Interrupt:
    name = "interrupt";
    break;
  case TransferMode::Polled:
    name = "polled";
    break;
  default:
    name = "unknown";
    break;
  }
  return name;
}

/** @brief What one port object offers (answer of `Capabilities()`). */
struct Capabilities {
  TransferMode mode{TransferMode::Polled}; ///< The path in use.
  bool zero_copy{false};                   ///< Typed `Lease` transfers are available.
  bool confirmed_abort{false};             ///< An abort waits for proof that the master stopped.
  std::size_t max_frame_bytes{0U}; ///< The largest frame this mode carries with chip-select held.
  std::size_t staging_bytes{0U};   ///< The size of the port-owned staging buffer (0: none).
  std::size_t align_bytes{1U};     ///< The alignment unit of the port's DMA buffers.
};

/** @brief The DMA part of a handler's health record. */
struct DmaHealth {
  TransferMode mode{TransferMode::Polled}; ///< The path serving now (never hidden).
  bool faulted{false};                     ///< A stop was not confirmed; `Recover()` is needed.
  uint32_t quarantined{0U};                ///< Buffers quarantined since start.
  uint32_t errors{0U};                     ///< Transfers that ended in an error.
};

/** @brief A port object that reports its capabilities (`Capabilities()` is `noexcept` and cheap).
 */
template <typename P>
concept ReportsCapabilities = requires(const P& port) {
  { port.Capabilities() } noexcept -> std::same_as<Capabilities>;
};

} // namespace kb::dma

#endif // KB_DMA_CAPABILITIES_HPP
