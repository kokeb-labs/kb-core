/**
 * @file service_key.hpp
 * @brief kb::handlers::ServiceKey: the pass that every mutating call of a handler core takes.
 *
 * The core is single-threaded (ADR-0009): it may only run on its bus's owner task or under its
 * guarded locks. A ServiceKey can only be made by the two access facades and the owner domain's
 * service pass, so code that reaches a handler any other way does not compile (`StartStep`,
 * `Recover`, `Execute`, ... all take a `const ServiceKey&`). Reads (`Latest`, `Health`,
 * `Startup`) need no key: they are lock-free copies.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_HANDLERS_COMMON_SERVICE_KEY_HPP
#define KB_HANDLERS_COMMON_SERVICE_KEY_HPP

#include <cstddef>

#include "kb/handlers/common/types.hpp"

namespace kb::handlers {

template <typename Bus>
class OwnerBusDomain;
template <typename H, typename Domain>
class Guarded;
template <typename H, typename Domain, std::size_t Depth>
class OwnerAccess;

/** @brief Proof that the caller runs in the one context allowed to touch a handler core. */
class ServiceKey {
public:
  ServiceKey(const ServiceKey&) = delete;
  ServiceKey& operator=(const ServiceKey&) = delete;
  ServiceKey(ServiceKey&&) = delete;
  ServiceKey& operator=(ServiceKey&&) = delete;
  ~ServiceKey() = default;

private:
  constexpr ServiceKey() noexcept = default;

  template <typename Bus>
  friend class OwnerBusDomain;
  template <typename H, typename Domain>
  friend class Guarded;
  template <typename H, typename Domain, std::size_t Depth>
  friend class OwnerAccess;
  /// The unit tests of the single-threaded core (components/lib/handlers/tests) define this peer.
  friend struct ServiceKeyTestPeer;
};

} // namespace kb::handlers

#endif // KB_HANDLERS_COMMON_SERVICE_KEY_HPP
