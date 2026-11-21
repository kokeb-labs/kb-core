/**
 * @file fake_spi.hpp
 * @brief kb::iface::host::FakeSpiDevice: an SPI device that records what was clocked out and
 * answers from a script, with fault injection, for host tests. Test code, outside the MISRA claim.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_INTERFACES_HOST_FAKE_SPI_HPP
#define KB_INTERFACES_HOST_FAKE_SPI_HPP

#include <cstddef>
#include <cstdint>
#include <span>

#include "kb/interfaces/spi.hpp"
#include "kb/utils/ring_buffer.hpp"
#include "kb/utils/static_vector.hpp"
#include "kb/utils/status.hpp"

namespace kb::iface::host {

/** @brief The host SPI device: MISO bytes come from Respond(), MOSI bytes are kept in Sent(). */
class FakeSpiDevice {
public:
  /** @brief Queues bytes the device answers with, in order; 0x00 once the script runs out. */
  [[nodiscard]] bool Respond(std::span<const uint8_t> bytes) noexcept {
    bool ok = true;
    for (const uint8_t b : bytes) {
      ok = ok && script_.Push(b);
    }
    return ok;
  }
  /** @brief The next transfer fails with `status` (Ok clears). */
  void FailNext(kb::Status status) noexcept {
    fail_ = status;
  }
  /** @brief Every byte clocked out so far (the first 512). */
  [[nodiscard]] std::span<const uint8_t> Sent() const noexcept {
    return sent_.AsSpan();
  }
  /** @brief Transfers completed. */
  [[nodiscard]] uint32_t Transfers() const noexcept {
    return transfers_;
  }
  /** @brief The clock last asked for. */
  [[nodiscard]] uint32_t ClockHz() const noexcept {
    return clock_hz_;
  }

  /** @brief SpiDevice::EnsureReady. */
  [[nodiscard]] kb::Status EnsureReady() noexcept {
    ready_ = true;
    return kb::Status::Ok;
  }
  /** @brief SpiDevice::Transfer. */
  [[nodiscard]] kb::Status Transfer(std::span<const uint8_t> tx, std::span<uint8_t> rx) noexcept {
    kb::Status s = kb::Status::Ok;
    if (!ready_) {
      s = kb::Status::NotReady;
    } else if (!kb::iface::SpiSpansValid(tx, rx)) {
      s = kb::Status::InvalidArgument;
    } else if (fail_ != kb::Status::Ok) {
      s = fail_;
      fail_ = kb::Status::Ok;
    } else {
      const std::size_t n = tx.empty() ? rx.size() : tx.size();
      for (std::size_t i = 0U; i < n; ++i) {
        static_cast<void>(sent_.PushBack(tx.empty() ? uint8_t{0U} : tx[i]));
        uint8_t answer = 0U;
        static_cast<void>(script_.Pop(answer));
        if (!rx.empty()) {
          rx[i] = answer;
        }
      }
      ++transfers_;
    }
    return s;
  }
  /** @brief SpiClockControl::SetClockHz; 0 or above 80 MHz is InvalidArgument. */
  [[nodiscard]] kb::Status SetClockHz(uint32_t hz) noexcept {
    constexpr uint32_t kMaxHz = 80000000U;
    const bool ok = (hz != 0U) && (hz <= kMaxHz);
    if (ok) {
      clock_hz_ = hz;
    }
    return ok ? kb::Status::Ok : kb::Status::InvalidArgument;
  }

private:
  kb::utils::RingBuffer<uint8_t, 512> script_;
  kb::utils::StaticVector<uint8_t, 512> sent_;
  kb::Status fail_{kb::Status::Ok};
  uint32_t transfers_{0U};
  uint32_t clock_hz_{1000000U};
  bool ready_{false};
};

static_assert(kb::iface::SpiClockControl<FakeSpiDevice>,
              "FakeSpiDevice conforms to SpiDevice and SpiClockControl");

} // namespace kb::iface::host

#endif // KB_INTERFACES_HOST_FAKE_SPI_HPP
