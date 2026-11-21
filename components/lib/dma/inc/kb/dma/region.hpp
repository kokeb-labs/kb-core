/**
 * @file region.hpp
 * @brief Memory regions as policies, not addresses: the `Region` concept, cache policies, DMA
 * masters, transfer directions, and the compile-time checks a port or a binding writes as
 * `static_assert`.
 *
 * kb-core knows no addresses. A board's generated code defines one type per memory the DMA can use
 * (`struct D2Sram { static constexpr ... }`); the type carries the cache policy, the cache line,
 * the alignment every buffer in it needs and the set of bus masters that can reach it.
 * `kb::dma::Buffer` is padded and aligned from those numbers.
 *
 * @code
 * struct SramRegion {
 *   static constexpr kb::dma::CachePolicy kPolicy = kb::dma::CachePolicy::NonCacheable;
 *   static constexpr std::size_t kLineBytes = 1U;
 *   static constexpr std::size_t kAlignBytes = 4U;
 *   static constexpr kb::dma::MasterSet kMasters = kb::dma::MasterBit(0U) | kb::dma::MasterBit(1U);
 * };
 * static_assert(kb::dma::Region<SramRegion>);
 * @endcode
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_DMA_REGION_HPP
#define KB_DMA_REGION_HPP

#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace kb::dma {

/**
 * @brief What sits between the CPU and the memory a DMA master reads and writes.
 *
 * - `Coherent`: no cache between them (Cortex-M parts without a data cache, the ESP32 internal SRAM
 *   of the parts without a cache): no maintenance, only barriers.
 * - `NonCacheable`: a memory-protection attribute makes the CPU bypass its cache for the region:
 * the same, and the board must have set the attribute (the port checks it at start).
 * - `Maintained`: cached write-back; the port cleans and invalidates whole lines at every ownership
 *   transfer.
 */
enum class CachePolicy : uint8_t {
  Coherent = 0,     ///< No cache between CPU and DMA.
  NonCacheable = 1, ///< The CPU bypasses its cache for this region (MPU or MMU attribute).
  Maintained = 2,   ///< Cached; the port cleans and invalidates by line.
};

/** @brief Which way a transfer moves data. */
enum class Direction : uint8_t {
  ToDevice = 0,   ///< Memory to peripheral (transmit): the port cleans before the DMA reads.
  FromDevice = 1, ///< Peripheral to memory (receive): the port invalidates around the DMA writes.
};

/** @brief One bit per DMA master (or other bus master) a port defines; see `MasterBit`. */
using MasterSet = uint32_t;

/** @brief The bit of master number `index` (0 to 31); a larger index gives 0 (no master). */
[[nodiscard]] constexpr MasterSet MasterBit(uint32_t index) noexcept {
  constexpr uint32_t kBits = 32U;
  return (index < kBits) ? (MasterSet{1U} << index) : MasterSet{0U};
}

/**
 * @brief What a memory-region tag type provides.
 *
 * - `kPolicy`: the `CachePolicy`.
 * - `kLineBytes`: the cache line in bytes (1 when `Coherent` or `NonCacheable`), a power of two.
 * - `kAlignBytes`: the alignment and padding unit of a buffer: at least the line, a power of two,
 *   and at least the DMA's own address rule and descriptor rule.
 * - `kMasters`: the bus masters that can reach the region (`MasterSet`).
 *
 * A `Maintained` region has a line of at least 2 bytes (a line of 1 would make maintenance
 * meaningless).
 */
template <typename R>
concept Region =
    requires {
      { R::kPolicy } -> std::convertible_to<CachePolicy>;
      { R::kLineBytes } -> std::convertible_to<std::size_t>;
      { R::kAlignBytes } -> std::convertible_to<std::size_t>;
      { R::kMasters } -> std::convertible_to<MasterSet>;
    } && (static_cast<std::size_t>(R::kLineBytes) > 0U) &&
    std::has_single_bit(static_cast<std::size_t>(R::kLineBytes)) &&
    std::has_single_bit(static_cast<std::size_t>(R::kAlignBytes)) &&
    (static_cast<std::size_t>(R::kAlignBytes) >= static_cast<std::size_t>(R::kLineBytes)) &&
    ((static_cast<CachePolicy>(R::kPolicy) != CachePolicy::Maintained) ||
     (static_cast<std::size_t>(R::kLineBytes) > 1U));

/** @brief `n` rounded up to a multiple of `unit` (a power of two, at least 1); saturates at the
 * largest multiple. */
[[nodiscard]] constexpr std::size_t RoundUp(std::size_t n, std::size_t unit) noexcept {
  const std::size_t mask = unit - 1U;
  const std::size_t largest = std::numeric_limits<std::size_t>::max() & ~mask;
  return (n > largest) ? largest : ((n + mask) & ~mask);
}

/** @brief True when `n` is a multiple of `unit` (a power of two, at least 1). */
[[nodiscard]] constexpr bool IsMultipleOf(std::size_t n, std::size_t unit) noexcept {
  return (n & (unit - 1U)) == 0U;
}

/** @brief True when the master set of region `R` contains every master in `masters`. */
template <MasterSet Masters, Region R>
inline constexpr bool kRegionReachableBy =
    (static_cast<MasterSet>(R::kMasters) & Masters) == Masters;

/**
 * @name Compile-time checks
 * Each is a function template whose body is a `static_assert`: call it from a binding or a port at
 * namespace scope (`constexpr bool kOk = (kb::dma::RequireFrameFits<A, B>(), true);`) or inside a
 * function; a violation is a build error with the message below.
 * @{
 */

/** @brief The driver's largest frame must fit the port's DMA limit. */
template <std::size_t DriverMaxBytes, std::size_t PortMaxBytes>
consteval void RequireFrameFits() {
  static_assert(DriverMaxBytes <= PortMaxBytes,
                "kb::dma: the driver's largest frame exceeds the port's DMA frame limit");
}

/** @brief Every master in `Masters` must reach region `R`. */
template <MasterSet Masters, Region R>
consteval void RequireReachable() {
  static_assert(kRegionReachableBy<Masters, R>,
                "kb::dma: a DMA master named here cannot reach this memory region");
}

/** @brief `Bytes` must be a whole number of units of region `R` (lines or alignment). */
template <std::size_t Bytes, Region R>
consteval void RequireWholeUnits() {
  static_assert(IsMultipleOf(Bytes, R::kAlignBytes),
                "kb::dma: the size is not a multiple of the region's alignment unit");
}

/** @brief The element type's size must equal what the board declares for the stream. */
template <typename Element, std::size_t DeclaredBytes>
consteval void RequireElementBytes() {
  static_assert(sizeof(Element) == DeclaredBytes,
                "kb::dma: the stream's element size differs from the board declaration");
}

/** @} */

} // namespace kb::dma

#endif // KB_DMA_REGION_HPP
