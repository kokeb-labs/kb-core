/**
 * @file bus_domain.hpp
 * @brief kb::handlers::BusDomain<Bus, ThreadModel>: one physical bus and its declared thread model
 * (ADR-0009), and BusHold, the RAII hold of a whole bus that `Hold(timeout)` returns (ADR-0015).
 *
 * - `BusDomain<Bus, ThreadModel::Owner>`: no mutex. The owner task calls `Service(now, a, b, ...)`
 *   each step, which services every OwnerAccess of the bus in order. A second `Service` while one
 * is running (another task, or a nested call) is refused with `Busy` and counted: that is the owner
 *   check, in every build. `Hold()` takes the same pass flag: it never waits, and a hold and a
 *   service pass exclude each other.
 * - `BusDomain<Bus, ThreadModel::Guarded>`: a kb::rtos::Mutex taken with a bounded wait by
 *   `Guarded<H>` (after the handler's own mutex) or by `Hold(timeout)`, with hold statistics and
 *   bus recovery.
 *
 * A `BusHold` is `[[nodiscard]]`, move-only and made only by `Hold()`; it releases the bus when it
 * is destroyed or `Release()`d, after which `Bus()` is null. `Bus()` on a temporary hold does not
 * compile (the reference would outlive the hold).
 *
 * `Bus` is the kb contract type of the bus (`kb::iface::I2cBus`, `CanBus`, ...) or, for a bus whose
 * devices each have their own handle (SPI chip selects), a tag type with
 * `static constexpr BusKind kKind`. A facade checks that its handler's chip is on that kind of bus.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_HANDLERS_COMMON_BUS_DOMAIN_HPP
#define KB_HANDLERS_COMMON_BUS_DOMAIN_HPP

#include <array>
#include <atomic>
#include <concepts>
#include <cstdint>
#include <limits>
#include <type_traits>

#include "kb/handlers/common/service_key.hpp"
#include "kb/handlers/common/types.hpp"
#include "kb/interfaces/interfaces.hpp"
#include "kb/rtos/mutex.hpp"
#include "kb/rtos/time.hpp"
#include "kb/utils/result.hpp"
#include "kb/utils/status.hpp"
#include "kb/utils/time.hpp"

namespace kb::handlers {

/** @brief A bus type that names its kind itself (a tag for buses reached per device). */
template <BusKind K>
struct BusTag {
  static constexpr BusKind kKind = K; ///< The kind of bus.
};

/** @brief True when `Bus` declares `kKind == K`. */
template <typename Bus, BusKind K>
concept TaggedBus = requires {
  { Bus::kKind } -> std::convertible_to<BusKind>;
  requires(Bus::kKind == K);
};

/** @brief True when `Bus` is a bus of kind K: it satisfies that kind's kb contract, or is tagged K.
 */
template <typename Bus, BusKind K>
concept BusOfKind = TaggedBus<Bus, K> || ((K == BusKind::I2c) && kb::iface::I2cBus<Bus>) ||
                    ((K == BusKind::Spi) && kb::iface::SpiDevice<Bus>) ||
                    ((K == BusKind::Uart) && kb::iface::UartPort<Bus>) ||
                    ((K == BusKind::Adc) && kb::iface::AdcChannel<Bus>) ||
                    ((K == BusKind::Can) && kb::iface::CanBus<Bus>) || (K == BusKind::Pio);

/** @brief A bus whose controller can free a stuck bus (`kb::Status RecoverBus() noexcept`). */
template <typename Bus>
concept RecoverableBus = requires(Bus& bus) {
  { bus.RecoverBus() } noexcept -> std::same_as<kb::Status>;
};

/** @brief Statistics of a guarded bus. */
struct BusStats {
  uint32_t holds{0U};         ///< Successful holds.
  uint32_t lock_timeouts{0U}; ///< Holds that could not get the bus in time.
  uint32_t max_hold_us{0U};   ///< Longest hold, microseconds (saturating).
  uint32_t recoveries{0U};    ///< RecoverBus() calls that succeeded.
};

/** @brief Statistics of an owner bus. */
struct OwnerStats {
  uint32_t passes{0U};     ///< Service passes completed.
  uint32_t violations{0U}; ///< Service calls and holds refused because a pass or hold was running.
  uint32_t holds{0U};      ///< Holds taken (Hold() between passes).
};

template <typename Domain>
class [[nodiscard]] BusHold;

/**
 * @brief A bus served by one owner task (model O): no mutex.
 * @tparam Bus The bus contract type or a BusTag.
 */
template <typename Bus>
class OwnerBusDomain {
public:
  static constexpr ThreadModel kModel = ThreadModel::Owner; ///< This domain's model.
  using BusType = Bus;                                      ///< The bus type.

  /** @brief A domain over `bus`; no I/O, no RTOS call. */
  constexpr explicit OwnerBusDomain(Bus& bus) noexcept : bus_(bus) {}
  OwnerBusDomain(const OwnerBusDomain&) = delete;
  OwnerBusDomain& operator=(const OwnerBusDomain&) = delete;
  OwnerBusDomain(OwnerBusDomain&&) = delete;
  OwnerBusDomain& operator=(OwnerBusDomain&&) = delete;
  ~OwnerBusDomain() = default;

  /** @brief The bus. */
  [[nodiscard]] Bus& Get() noexcept {
    return bus_;
  }

  /**
   * @brief One service pass of the owner task at `now`: each access's Service(), in order.
   * @return Busy (nothing serviced, a violation counted) when another pass is running.
   */
  template <typename... Accesses>
  [[nodiscard]] kb::Result<void> Service(kb::Instant now, Accesses&... accesses) noexcept {
    kb::Result<void> result = kb::Result<void>::Success();
    bool idle = false;
    if (!in_service_.compare_exchange_strong(idle, true, std::memory_order_acq_rel)) {
      static_cast<void>(violations_.fetch_add(1U, std::memory_order_relaxed));
      result = kb::Result<void>::Failure(kb::Status::Busy);
    } else {
      const ServiceKey key{};
      static_cast<void>(now); // unused when the pass has no access
      // Braced list: evaluated left to right, one Service() per access.
      const std::array<bool, sizeof...(Accesses)> serviced{accesses.Service(key, now)...};
      static_cast<void>(serviced);
      static_cast<void>(passes_.fetch_add(1U, std::memory_order_relaxed));
      in_service_.store(false, std::memory_order_release);
    }
    return result;
  }

  /**
   * @brief Holds the bus for the owner task's own multi-step work between passes (a raw sequence,
   * held handler calls through OwnerAccess::Held). Never waits: `timeout` is accepted for the one
   * API of both models and unused.
   * @return A hold; it does not own the bus (a violation counted) while a pass or another hold
   * runs.
   */
  [[nodiscard]] BusHold<OwnerBusDomain> Hold(kb::Milliseconds timeout) noexcept {
    static_cast<void>(timeout); // an owner hold never waits
    bool idle = false;
    const bool owned = in_service_.compare_exchange_strong(idle, true, std::memory_order_acq_rel);
    if (owned) {
      static_cast<void>(holds_.fetch_add(1U, std::memory_order_relaxed));
    } else {
      static_cast<void>(violations_.fetch_add(1U, std::memory_order_relaxed));
    }
    return BusHold<OwnerBusDomain>{*this, owned};
  }

  /** @brief A copy of the statistics. */
  [[nodiscard]] OwnerStats Stats() const noexcept {
    OwnerStats s{};
    s.passes = passes_.load(std::memory_order_relaxed);
    s.violations = violations_.load(std::memory_order_relaxed);
    s.holds = holds_.load(std::memory_order_relaxed);
    return s;
  }

  /** @brief Frees a stuck bus: only from the owner task (between passes). */
  [[nodiscard]] kb::Result<void> RecoverBus() noexcept {
    kb::Result<void> result = kb::Result<void>::Failure(kb::Status::Unsupported);
    if constexpr (RecoverableBus<Bus>) {
      result = kb::Result<void>::FromStatus(bus_.RecoverBus());
    }
    return result;
  }

private:
  friend class BusHold<OwnerBusDomain>;

  void release(uint64_t held_us) noexcept {
    static_cast<void>(held_us); // an owner bus keeps no hold times
    in_service_.store(false, std::memory_order_release);
  }

  Bus& bus_;
  std::atomic<bool> in_service_{false};
  std::atomic<uint32_t> passes_{0U};
  std::atomic<uint32_t> violations_{0U};
  std::atomic<uint32_t> holds_{0U};
};

/**
 * @brief A bus shared by several tasks under a mutex (model G).
 * @tparam Bus The bus contract type or a BusTag.
 */
template <typename Bus>
class GuardedBusDomain {
public:
  static constexpr ThreadModel kModel = ThreadModel::Guarded; ///< This domain's model.
  using BusType = Bus;                                        ///< The bus type.

  /** @brief A domain over `bus`; no I/O, no RTOS call (Create() makes the mutex). */
  constexpr explicit GuardedBusDomain(Bus& bus) noexcept : bus_(bus) {}
  GuardedBusDomain(const GuardedBusDomain&) = delete;
  GuardedBusDomain& operator=(const GuardedBusDomain&) = delete;
  GuardedBusDomain(GuardedBusDomain&&) = delete;
  GuardedBusDomain& operator=(GuardedBusDomain&&) = delete;
  ~GuardedBusDomain() = default;

  /** @brief Creates the bus mutex (after the scheduler runs). `AlreadyStarted` on a second call. */
  [[nodiscard]] kb::Result<void> Create() noexcept {
    return mutex_.Create();
  }

  /** @brief The bus. */
  [[nodiscard]] Bus& Get() noexcept {
    return bus_;
  }

  /**
   * @brief Holds the bus mutex, waiting at most `timeout` for it (a whole multi-transfer driver
   * call, a held handler sequence through Guarded::Hold, a HAL sequence).
   * @return A hold; it does not own the bus (a lock timeout counted) when the wait ran out or the
   * mutex was not created.
   */
  [[nodiscard]] BusHold<GuardedBusDomain> Hold(kb::Milliseconds timeout) noexcept {
    const bool owned = lock(timeout);
    return BusHold<GuardedBusDomain>{*this, owned};
  }

  /** @brief A copy of the statistics. */
  [[nodiscard]] BusStats Stats() const noexcept {
    BusStats s{};
    s.holds = holds_.load(std::memory_order_relaxed);
    s.lock_timeouts = timeouts_.load(std::memory_order_relaxed);
    s.max_hold_us = max_hold_us_.load(std::memory_order_relaxed);
    s.recoveries = recoveries_.load(std::memory_order_relaxed);
    return s;
  }

  /**
   * @brief Frees a stuck bus under the bus lock, waiting at most `timeout` for it.
   * @return Unsupported when the bus cannot recover; Timeout when the lock was not had in time.
   */
  [[nodiscard]] kb::Result<void> RecoverBus(kb::Milliseconds timeout) noexcept {
    static_cast<void>(timeout); // used only when the bus can recover
    kb::Result<void> result = kb::Result<void>::Failure(kb::Status::Unsupported);
    if constexpr (RecoverableBus<Bus>) {
      const BusHold<GuardedBusDomain> hold = Hold(timeout);
      if (!hold.OwnsLock()) {
        result = kb::Result<void>::Failure(kb::Status::Timeout);
      } else {
        result = kb::Result<void>::FromStatus(bus_.RecoverBus());
        if (result.IsOk()) {
          static_cast<void>(recoveries_.fetch_add(1U, std::memory_order_relaxed));
        }
      }
    }
    return result;
  }

private:
  friend class BusHold<GuardedBusDomain>;

  [[nodiscard]] bool lock(kb::Milliseconds timeout) noexcept {
    const bool owned = mutex_.TryLockFor(timeout);
    if (owned) {
      static_cast<void>(holds_.fetch_add(1U, std::memory_order_relaxed));
    } else {
      static_cast<void>(timeouts_.fetch_add(1U, std::memory_order_relaxed));
    }
    return owned;
  }

  void release(uint64_t held_us) noexcept {
    constexpr uint64_t kMax = std::numeric_limits<uint32_t>::max();
    const auto clipped = static_cast<uint32_t>((held_us > kMax) ? kMax : held_us);
    if (clipped > max_hold_us_.load(std::memory_order_relaxed)) {
      max_hold_us_.store(clipped, std::memory_order_relaxed);
    }
    mutex_.Unlock();
  }

  Bus& bus_;
  kb::rtos::Mutex mutex_;
  std::atomic<uint32_t> holds_{0U};
  std::atomic<uint32_t> timeouts_{0U};
  std::atomic<uint32_t> max_hold_us_{0U};
  std::atomic<uint32_t> recoveries_{0U};
};

/**
 * @brief A bus and its declared thread model (ADR-0009): `BusDomain<Bus, ThreadModel::Owner>` is an
 * OwnerBusDomain, `BusDomain<Bus, ThreadModel::Guarded>` a GuardedBusDomain.
 */
template <typename Bus, ThreadModel Model>
using BusDomain =
    std::conditional_t<(Model == ThreadModel::Owner), OwnerBusDomain<Bus>, GuardedBusDomain<Bus>>;

/**
 * @brief The hold of a domain's whole bus (ADR-0015): on a guarded bus the bus mutex, taken within
 * a bounded wait; on an owner bus the pass flag, taken at once or not at all. Check OwnsLock().
 *
 * Made only by `Domain::Hold(timeout)`; `[[nodiscard]]`, so a hold that is discarded (and released
 * at once) does not compile. Move-only: a moved-from hold owns nothing. The bus is released by the
 * destructor or by `Release()` (idempotent); after that `Bus()` is null. Never held across another
 * bus: every wait for a bus is bounded, so a second hold taken in the opposite order elsewhere ends
 * in a timeout, never a hang.
 * @tparam Domain An OwnerBusDomain or GuardedBusDomain.
 */
template <typename Domain>
class [[nodiscard]] BusHold {
public:
  BusHold(const BusHold&) = delete;
  BusHold& operator=(const BusHold&) = delete;
  BusHold& operator=(BusHold&&) = delete;
  /** @brief Takes over `other`'s hold; `other` owns nothing afterwards. */
  BusHold(BusHold&& other) noexcept
      : domain_(other.domain_), start_(other.start_), owns_(other.owns_) {
    other.owns_ = false;
  }
  /** @brief Releases the bus when it is held. */
  ~BusHold() {
    Release();
  }

  /** @brief True while this hold owns the bus. */
  [[nodiscard]] bool OwnsLock() const noexcept {
    return owns_;
  }

  /** @brief True when this hold was taken on `domain`. */
  [[nodiscard]] bool IsOn(const Domain& domain) const noexcept {
    return domain_ == &domain;
  }

  /** @brief Releases the bus now (the hold's time is recorded); nothing when not owned. */
  void Release() noexcept {
    if (owns_) {
      owns_ = false;
      domain_->release(kb::utils::ElapsedUs(kb::rtos::Now(), start_));
    }
  }

  /** @brief The bus while it is held; null when not owned or released. */
  [[nodiscard]] typename Domain::BusType* Bus() & noexcept {
    return owns_ ? &domain_->bus_ : nullptr;
  }
  /// A temporary hold is released at the end of the expression: its bus must not be kept.
  typename Domain::BusType* Bus() && = delete;

private:
  friend Domain;

  BusHold(Domain& domain, bool owned) noexcept
      : domain_(&domain), start_(kb::rtos::Now()), owns_(owned) {}

  Domain* domain_;
  kb::Instant start_;
  bool owns_;
};

} // namespace kb::handlers

#endif // KB_HANDLERS_COMMON_BUS_DOMAIN_HPP
