/**
 * @file assignment.hpp
 * @brief What a board declares about its DMA, as the board generator writes it: one
 * `StreamAssignment` per DMA stream, the interrupt limits of the RTOS, the DMA limits of each bus
 * and the shape of each stream, with the compile-time checks a port or a binding runs on them.
 *
 * The board generator (`kb board gen`, ADR-0033) resolves `board.yml` against the routing data of
 * the part and writes these values into `board_dma.hpp` as `constexpr` members of one tag type per
 * stream, bus or board; the checker (`kb image check`) has already refused every conflict. A port
 * receives a resolved assignment and only checks it: it never searches for a free stream at run
 * time.
 *
 * @code
 * struct Spi1Rx {   // generated
 *   static constexpr kb::dma::StreamAssignment kAssignment{1U, 0U, 37U,
 *       kb::dma::ChannelPriority::High, 5U, kb::dma::Notify::Task, kb::dma::IrqUse::All,
 *       kb::dma::ChannelMode::Normal};
 * };
 * struct Rtos { static constexpr kb::dma::InterruptLimits kLimits{5U, 4U}; };   // generated
 * constexpr bool kOk = (kb::dma::RequireInterruptTier<Spi1Rx, Rtos>(), true);
 * @endcode
 *
 * The meaning of `engine`, `stream` and `request` belongs to the port (its `docs/dma.md` and its
 * routing data): on STM32 the engine numbers DMA1, DMA2, BDMA and MDMA, and the request is the
 * DMAMUX request line (G4, H7, L4+), the channel selection (F4) or the `CSELR` value (L4).
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_DMA_ASSIGNMENT_HPP
#define KB_DMA_ASSIGNMENT_HPP

#include <cstddef>
#include <cstdint>

namespace kb::dma {

/** @brief How a stream's interrupt tells the owner task that a frame is ready. */
enum class Notify : uint8_t {
  None = 0, ///< It does not: the interrupt may run above the RTOS limit; the owner polls each pass.
  Task = 1, ///< It signals the owner's event: the interrupt must be at or below the RTOS limit.
};

/** @brief Which of a stream's interrupts are enabled. */
enum class IrqUse : uint8_t {
  All = 0, ///< Half, complete and error interrupts.
  ErrorsOnly =
      1, ///< Error interrupts only (a transmit stream whose completion the receive implies).
};

/** @brief The arbitration priority of a stream inside its engine. */
enum class ChannelPriority : uint8_t {
  Low = 0,
  Medium = 1,
  High = 2,
  VeryHigh = 3,
};

/** @brief Whether a stream stops at its end or wraps. */
enum class ChannelMode : uint8_t {
  Normal = 0,   ///< One transfer, then the stream disables itself.
  Circular = 1, ///< The stream wraps at its end (half and complete interrupts per pass).
};

/** @brief One DMA stream bound to one peripheral request, resolved and checked by the generator. */
struct StreamAssignment {
  uint8_t engine{0U};   ///< The port's engine number (0: none).
  uint8_t stream{0U};   ///< The stream or channel inside the engine.
  uint16_t request{0U}; ///< The request line, channel selection or `CSELR` value (port-defined).
  ChannelPriority priority{ChannelPriority::Medium}; ///< Arbitration inside the engine.
  uint8_t irq_priority{0U};    ///< The interrupt priority value (lower is more urgent on Cortex-M).
  Notify notify{Notify::Task}; ///< How the owner learns of a frame.
  IrqUse irq{IrqUse::All};     ///< Which interrupts are enabled.
  ChannelMode mode{ChannelMode::Normal}; ///< Normal or circular.
};

/** @brief What the RTOS allows interrupts to do. */
struct InterruptLimits {
  uint8_t max_syscall_priority{0U}; ///< The most urgent priority value that may call the RTOS.
  uint8_t priority_bits{8U};        ///< Implemented priority bits (values are below 2^bits).
};

/** @brief The DMA facts of one bus, generated per bus from the board and the port's limits. */
struct BusLimits {
  bool dma{false};                    ///< The board gave the bus DMA streams.
  std::size_t max_frame_dma{0U};      ///< The largest frame by DMA (the staging capacity).
  std::size_t max_frame_no_dma{0U};   ///< The largest frame without DMA (interrupt or FIFO limit).
  std::size_t rx_length_multiple{1U}; ///< Receive lengths the DMA rounds up to (4 on ESP32 SPI).
  std::size_t align_bytes{1U};        ///< The alignment unit of the bus's staging buffers.

  /** @brief The largest frame the bus carries in the mode the board gave it. */
  [[nodiscard]] constexpr std::size_t MaxFrame() const noexcept {
    return dma ? max_frame_dma : max_frame_no_dma;
  }
};

/** @brief The declared shape of a stream: frames in the ring and the bytes of one frame. */
struct StreamShape {
  std::size_t frames{2U};        ///< Slots of the ring (2 for a two-half circular buffer).
  std::size_t element_bytes{1U}; ///< Bytes of one sample.
  std::size_t elements{1U};      ///< Samples in one frame.

  /** @brief Bytes of one frame. */
  [[nodiscard]] constexpr std::size_t FrameBytes() const noexcept {
    return element_bytes * elements;
  }
};

/**
 * @brief True when `priority` fits the limits and, for `Notify::Task`, is not more urgent than the
 * RTOS allows (a value at or above `max_syscall_priority`).
 */
[[nodiscard]] constexpr bool InterruptTierAllowed(uint8_t priority, Notify notify,
                                                  const InterruptLimits& limits) noexcept {
  constexpr uint32_t kOne = 1U;
  const uint32_t levels = (limits.priority_bits >= 8U) ? 256U : (kOne << limits.priority_bits);
  const bool in_range = static_cast<uint32_t>(priority) < levels;
  const bool rtos_ok = (notify == Notify::None) || (priority >= limits.max_syscall_priority);
  return in_range && rtos_ok;
}

/**
 * @name Compile-time checks
 * Function templates whose body is a `static_assert`, as in region.hpp. `S` is a tag type with
 * `kAssignment`, `L` one with `kLimits`, `B` one with `kBus`, `D` one with `kShape`.
 * @{
 */

/** @brief The stream's interrupt priority must suit its notification style and the RTOS. */
template <typename S, typename L>
consteval void RequireInterruptTier() {
  static_assert(
      InterruptTierAllowed(S::kAssignment.irq_priority, S::kAssignment.notify, L::kLimits),
      "kb::dma: a stream that notifies a task runs above the RTOS syscall limit, or its "
      "priority is outside the implemented bits");
}

/** @brief Two streams (of one peripheral) must share one interrupt priority. */
template <typename S1, typename S2>
consteval void RequireSamePriority() {
  static_assert(S1::kAssignment.irq_priority == S2::kAssignment.irq_priority,
                "kb::dma: the streams of one peripheral run at different interrupt priorities");
}

/** @brief The driver's largest frame must fit the bus in the mode the board gave it. */
template <typename B, std::size_t DriverMaxBytes>
consteval void RequireBusCarries() {
  static_assert(DriverMaxBytes <= B::kBus.MaxFrame(),
                "kb::dma: the driver's largest frame exceeds what this bus carries (give the bus "
                "DMA in board.yml, or a larger staging buffer)");
}

/** @brief The handler's frame type and ring must match the board's stream declaration. */
template <typename D, typename Frame, std::size_t Frames>
consteval void RequireShape() {
  static_assert(sizeof(Frame) == D::kShape.FrameBytes(),
                "kb::dma: the stream's frame size differs from the board declaration");
  static_assert(Frames == D::kShape.frames,
                "kb::dma: the stream's frame count differs from the board declaration");
}

/** @} */

} // namespace kb::dma

#endif // KB_DMA_ASSIGNMENT_HPP
