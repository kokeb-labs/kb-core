/**
 * @file cache.hpp
 * @brief The cache-maintenance hooks a port provides: `Clean`, `Invalidate`, `Barrier`.
 *
 * kb::dma calls these at ownership transfers and nowhere else (buffer.hpp): drivers and handlers
 * never touch a cache. Every range handed to a hook is made of whole cache lines that one
 * `kb::dma::Buffer` owns, so an invalidate can never destroy a neighbour.
 *
 * | Hook | Meaning |
 * | --- | --- |
 * | `Clean(span)` | Write back every dirty line of the range to memory (Cortex-M7:
 * `SCB_CleanDCache_by_Addr`; ESP-IDF: `esp_cache_msync` C2M) | | `Invalidate(span)` | Discard every
 * line of the range; the next CPU read goes to memory (M7: `SCB_InvalidateDCache_by_Addr`; ESP-IDF:
 * M2C) | | `Barrier()` | Order the maintenance and the CPU's earlier stores before the DMA is
 * started (`DSB`) |
 *
 * A port on a `Coherent` or `NonCacheable` region may use `kb::dma::NoCacheOps`.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_DMA_CACHE_HPP
#define KB_DMA_CACHE_HPP

#include <concepts>
#include <cstddef>
#include <span>

namespace kb::dma {

/** @brief What a port provides for cache maintenance (see the file note). All three are `noexcept`.
 */
template <typename Ops>
concept CacheOps = requires(Ops& ops, std::span<const std::byte> clean_range,
                            std::span<std::byte> invalidate_range) {
  { ops.Clean(clean_range) } noexcept;
  { ops.Invalidate(invalidate_range) } noexcept;
  { ops.Barrier() } noexcept;
};

/** @brief Maintenance that does nothing: for ports whose regions have no cache between CPU and DMA.
 */
class NoCacheOps {
public:
  /** @brief No cache: nothing to write back. */
  static void Clean(std::span<const std::byte> range) noexcept {
    static_cast<void>(range);
  }
  /** @brief No cache: nothing to discard. */
  static void Invalidate(std::span<std::byte> range) noexcept {
    static_cast<void>(range);
  }
  /** @brief No cache: nothing to order. */
  static void Barrier() noexcept {}
};

} // namespace kb::dma

#endif // KB_DMA_CACHE_HPP
