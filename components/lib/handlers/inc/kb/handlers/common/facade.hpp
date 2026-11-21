/**
 * @file facade.hpp
 * @brief What both access facades check about a handler and its bus (ADR-0009), and the handler
 * operations they run, as plain function objects.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_HANDLERS_COMMON_FACADE_HPP
#define KB_HANDLERS_COMMON_FACADE_HPP

#include <concepts>
#include <cstdint>

#include "kb/handlers/common/bus_domain.hpp"
#include "kb/handlers/common/service_key.hpp"
#include "kb/handlers/common/types.hpp"
#include "kb/rtos/time.hpp"
#include "kb/utils/result.hpp"

namespace kb::handlers {

/** @brief A handler type: a HandlerCore with its declared bus, plan and owner requirement. */
template <typename H>
concept Handler = requires(H& h, const H& ch, const ServiceKey& key, kb::Instant now,
                           const typename H::CommandType& command) {
  typename H::SnapshotType;
  typename H::CommandType;
  typename H::ReplyType;
  { H::kRequiresOwner } -> std::convertible_to<bool>;
  { H::kBus } -> std::convertible_to<BusKind>;
  { h.StartStep(key, now) } noexcept;
  { h.EnsureInitialized(key) } noexcept;
  { h.Recover(key, now) } noexcept;
  { h.Execute(key, command, now) } noexcept;
  { h.Sample(key, now) } noexcept;
  { ch.Latest() } noexcept;
  { ch.Health() } noexcept;
};

/** @brief The checks every facade makes: a handler, and a domain of the right kind of bus. */
template <typename H, typename Domain>
concept HandlerOnDomain = Handler<H> && BusOfKind<typename Domain::BusType, H::kBus>;

/** @brief The handler operations a facade runs in its one allowed context. */
namespace ops {

/** @brief `EnsureInitialized()`. */
struct Start {
  template <typename H>
  [[nodiscard]] kb::Result<void> operator()(H& h, const ServiceKey& key) const noexcept {
    return h.EnsureInitialized(key);
  }
};

/** @brief `Recover()` then the plan again. */
struct Recover {
  template <typename H>
  [[nodiscard]] kb::Result<void> operator()(H& h, const ServiceKey& key) const noexcept {
    kb::Result<void> result = h.Recover(key, kb::rtos::Now());
    if (result.IsOk()) {
      result = h.EnsureInitialized(key);
    }
    return result;
  }
};

/** @brief `Deinitialize()`. */
struct Stop {
  template <typename H>
  [[nodiscard]] kb::Result<void> operator()(H& h, const ServiceKey& key) const noexcept {
    return h.Deinitialize(key);
  }
};

/** @brief `Execute(command)`. */
struct Execute {
  template <typename H>
  [[nodiscard]] kb::Result<typename H::ReplyType> operator()(
      H& h, const ServiceKey& key, const typename H::CommandType& command) const noexcept {
    return h.Execute(key, command, kb::rtos::Now());
  }
};

/** @brief `Sample()`. */
struct Sample {
  template <typename H>
  [[nodiscard]] kb::Result<typename H::SnapshotType> operator()(
      H& h, const ServiceKey& key) const noexcept {
    return h.Sample(key, kb::rtos::Now());
  }
};

} // namespace ops

/** @brief Whole milliseconds left of `timeout` after `elapsed_us` (0 once spent). */
[[nodiscard]] constexpr kb::Milliseconds Remaining(kb::Milliseconds timeout,
                                                   uint64_t elapsed_us) noexcept {
  constexpr uint64_t kPerMs = 1000U;
  const uint64_t spent = elapsed_us / kPerMs;
  const uint64_t total = timeout.Count();
  return kb::Milliseconds{static_cast<uint32_t>((spent >= total) ? 0U : (total - spent))};
}

} // namespace kb::handlers

#endif // KB_HANDLERS_COMMON_FACADE_HPP
