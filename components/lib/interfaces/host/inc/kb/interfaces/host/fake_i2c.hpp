/**
 * @file fake_i2c.hpp
 * @brief kb::iface::host::FakeI2cBus: an I2C controller with register-file devices and fault
 * injection, for host tests. Test code, outside the MISRA claim.
 *
 * Up to four devices, each a 256-byte register file with an auto-incrementing register pointer (the
 * common I2C register model): a write's first byte sets the pointer, the rest are stored from it; a
 * read returns bytes from the pointer on. Faults: FailNext(failure, count) makes the next `count`
 * transfers fail with that kind.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_INTERFACES_HOST_FAKE_I2C_HPP
#define KB_INTERFACES_HOST_FAKE_I2C_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "kb/interfaces/i2c.hpp"
#include "kb/utils/status.hpp"

namespace kb::iface::host {

/// One simulated device on a FakeI2cBus.
struct FakeI2cDevice {
  uint8_t address{0U};             ///< 7-bit address; 0 marks an empty slot.
  std::array<uint8_t, 256> regs{}; ///< The register file.
  uint8_t pointer{0U};             ///< The register pointer.
};

/** @brief The host I2C controller (see the file note). */
class FakeI2cBus {
public:
  /** @brief Adds a device at `address`; false when the address is invalid, taken, or all slots are
   * used. */
  [[nodiscard]] bool AddDevice(uint8_t address) noexcept {
    bool added = false;
    if ((address >= 0x08U) && (address <= 0x77U) && (find(address) == nullptr)) {
      for (FakeI2cDevice& d : devices_) {
        if ((!added) && (d.address == 0U)) {
          d.address = address;
          added = true;
        }
      }
    }
    return added;
  }
  /** @brief The device at `address`, or null. */
  [[nodiscard]] FakeI2cDevice* Device(uint8_t address) noexcept {
    return find(address);
  }
  /** @brief The next `count` transfers fail with `failure` (None clears). */
  void FailNext(I2cFailure failure, uint32_t count = 1U) noexcept {
    inject_ = failure;
    inject_count_ = (failure == I2cFailure::None) ? 0U : count;
  }
  /** @brief EnsureReady fails with NotReady while `fail` is true. */
  void FailEnsureReady(bool fail) noexcept {
    fail_ready_ = fail;
  }
  /** @brief Transfers attempted since construction. */
  [[nodiscard]] uint32_t Transfers() const noexcept {
    return transfers_;
  }
  /** @brief True after a successful EnsureReady. */
  [[nodiscard]] bool Ready() const noexcept {
    return ready_;
  }

  /** @brief I2cBus::EnsureReady. */
  [[nodiscard]] kb::Status EnsureReady() noexcept {
    ready_ = !fail_ready_;
    return ready_ ? kb::Status::Ok : kb::Status::NotReady;
  }
  /** @brief I2cBus::Write. */
  [[nodiscard]] kb::Status Write(uint8_t address, std::span<const uint8_t> tx) noexcept {
    FakeI2cDevice* d = nullptr;
    kb::Status s = begin(address, d);
    if ((s == kb::Status::Ok) && (d != nullptr) && (!tx.empty())) {
      d->pointer = tx[0];
      for (const uint8_t byte : tx.subspan(1U)) {
        d->regs[d->pointer] = byte;
        ++d->pointer;
      }
    }
    return s;
  }
  /** @brief I2cBus::WriteRead. */
  [[nodiscard]] kb::Status WriteRead(uint8_t address, std::span<const uint8_t> tx,
                                     std::span<uint8_t> rx) noexcept {
    kb::Status s = Write(address, tx);
    if (s == kb::Status::Ok) {
      --transfers_; // one transfer with a repeated START
      s = Read(address, rx);
    }
    return s;
  }
  /** @brief I2cBus::Read. */
  [[nodiscard]] kb::Status Read(uint8_t address, std::span<uint8_t> rx) noexcept {
    FakeI2cDevice* d = nullptr;
    const kb::Status s = begin(address, d);
    if ((s == kb::Status::Ok) && (d != nullptr)) {
      for (uint8_t& byte : rx) {
        byte = d->regs[d->pointer];
        ++d->pointer;
      }
    }
    return s;
  }
  /** @brief I2cBus::LastFailure. */
  [[nodiscard]] I2cFailure LastFailure() const noexcept {
    return last_;
  }

private:
  [[nodiscard]] FakeI2cDevice* find(uint8_t address) noexcept {
    FakeI2cDevice* found = nullptr;
    for (FakeI2cDevice& d : devices_) {
      if ((found == nullptr) && (d.address != 0U) && (d.address == address)) {
        found = &d;
      }
    }
    return found;
  }

  [[nodiscard]] static kb::Status statusOf(I2cFailure failure) noexcept {
    kb::Status s = kb::Status::BusError;
    switch (failure) {
    case I2cFailure::None:
      s = kb::Status::Ok;
      break;
    case I2cFailure::AddressNack:
      s = kb::Status::NoDevice;
      break;
    case I2cFailure::DataNack:
      s = kb::Status::Nack;
      break;
    case I2cFailure::Timeout:
      s = kb::Status::Timeout;
      break;
    case I2cFailure::Bus:
    case I2cFailure::BusStuck:
    default:
      s = kb::Status::BusError;
      break;
    }
    return s;
  }

  [[nodiscard]] kb::Status begin(uint8_t address, FakeI2cDevice*& device) noexcept {
    ++transfers_;
    I2cFailure failure = I2cFailure::None;
    if (!ready_) {
      failure = I2cFailure::Bus;
    } else if (inject_count_ > 0U) {
      --inject_count_;
      failure = inject_;
    } else {
      device = find(address);
      failure = (device == nullptr) ? I2cFailure::AddressNack : I2cFailure::None;
    }
    last_ = failure;
    return ready_ ? statusOf(failure) : kb::Status::NotReady;
  }

  std::array<FakeI2cDevice, 4> devices_{};
  I2cFailure last_{I2cFailure::None};
  I2cFailure inject_{I2cFailure::None};
  uint32_t inject_count_{0U};
  uint32_t transfers_{0U};
  bool ready_{false};
  bool fail_ready_{false};
};

static_assert(kb::iface::I2cBus<FakeI2cBus>, "FakeI2cBus conforms to I2cBus");

} // namespace kb::iface::host

#endif // KB_INTERFACES_HOST_FAKE_I2C_HPP
