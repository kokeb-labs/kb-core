/**
 * @file fake_uart.hpp
 * @brief kb::iface::host::FakeUart: a UART with a receive script, a transmit log and fault
 * injection, for host tests. Test code, outside the MISRA claim.
 *
 * Read() returns what Feed() queued (up to the span's size); with nothing queued it fails with
 * Timeout at once (the timeout is recorded, not waited). An optional responder is called after
 * every Write, so a test can answer a request with a reply (`OnWrite`).
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_INTERFACES_HOST_FAKE_UART_HPP
#define KB_INTERFACES_HOST_FAKE_UART_HPP

#include <cstddef>
#include <cstdint>
#include <span>

#include "kb/interfaces/uart.hpp"
#include "kb/utils/result.hpp"
#include "kb/utils/ring_buffer.hpp"
#include "kb/utils/static_vector.hpp"
#include "kb/utils/status.hpp"
#include "kb/utils/time.hpp"

namespace kb::iface::host {

/** @brief The host UART (see the file note). */
class FakeUart {
public:
  /// Called after each successful Write with the bytes written.
  using Responder = void (*)(FakeUart& uart, std::span<const uint8_t> written,
                             void* context) noexcept;

  /** @brief Queues received bytes; false when the 512-byte receive buffer is full. */
  [[nodiscard]] bool Feed(std::span<const uint8_t> bytes) noexcept {
    bool ok = true;
    for (const uint8_t b : bytes) {
      ok = ok && rx_.Push(b);
    }
    return ok;
  }
  /** @brief Installs a responder (null removes it). */
  void OnWrite(Responder responder, void* context) noexcept {
    responder_ = responder;
    context_ = context;
  }
  /** @brief The next Write fails with `status` (Ok clears). */
  void FailNextWrite(kb::Status status) noexcept {
    fail_write_ = status;
  }
  /** @brief Every byte written so far (the first 512). */
  [[nodiscard]] std::span<const uint8_t> Written() const noexcept {
    return tx_.AsSpan();
  }
  /** @brief Forgets the transmit log. */
  void ClearWritten() noexcept {
    tx_.Clear();
  }
  /** @brief The timeout of the last Read. */
  [[nodiscard]] kb::Milliseconds LastTimeout() const noexcept {
    return last_timeout_;
  }
  /** @brief The baud rate last set. */
  [[nodiscard]] uint32_t Baud() const noexcept {
    return baud_;
  }

  /** @brief UartPort::EnsureReady. */
  [[nodiscard]] kb::Status EnsureReady() noexcept {
    ready_ = true;
    return kb::Status::Ok;
  }
  /** @brief UartPort::Write. */
  [[nodiscard]] kb::Status Write(std::span<const uint8_t> tx) noexcept {
    kb::Status s = ready_ ? kb::Status::Ok : kb::Status::NotReady;
    if ((s == kb::Status::Ok) && (fail_write_ != kb::Status::Ok)) {
      s = fail_write_;
      fail_write_ = kb::Status::Ok;
    }
    if (s == kb::Status::Ok) {
      for (const uint8_t b : tx) {
        static_cast<void>(tx_.PushBack(b));
      }
      if (responder_ != nullptr) {
        responder_(*this, tx, context_);
      }
    }
    return s;
  }
  /** @brief UartPort::Read. */
  [[nodiscard]] kb::Result<std::size_t> Read(std::span<uint8_t> rx,
                                             kb::Milliseconds timeout) noexcept {
    last_timeout_ = timeout;
    std::size_t n = 0U;
    uint8_t b = 0U;
    while ((n < rx.size()) && rx_.Pop(b)) {
      rx[n] = b;
      ++n;
    }
    kb::Result<std::size_t> r = kb::Result<std::size_t>::Success(n);
    if (!ready_) {
      r = kb::Result<std::size_t>::Failure(kb::Status::NotReady);
    } else if ((n == 0U) && (!rx.empty())) {
      r = kb::Result<std::size_t>::Failure(kb::Status::Timeout);
    } else {
      // Bytes arrived, or nothing was asked for.
    }
    return r;
  }
  /** @brief UartPort::FlushInput. */
  [[nodiscard]] kb::Status FlushInput() noexcept {
    rx_.Clear();
    return kb::Status::Ok;
  }
  /** @brief UartBaudControl::SetBaud; 0 is InvalidArgument. */
  [[nodiscard]] kb::Status SetBaud(uint32_t baud) noexcept {
    const bool ok = baud != 0U;
    if (ok) {
      baud_ = baud;
    }
    return ok ? kb::Status::Ok : kb::Status::InvalidArgument;
  }
  /** @brief UartTxDrain::WaitTxDone: the fake sends at once. */
  [[nodiscard]] kb::Status WaitTxDone(kb::Milliseconds timeout) const noexcept {
    static_cast<void>(timeout);
    return ready_ ? kb::Status::Ok : kb::Status::NotReady;
  }

private:
  kb::utils::RingBuffer<uint8_t, 512> rx_;
  kb::utils::StaticVector<uint8_t, 512> tx_;
  Responder responder_{nullptr};
  void* context_{nullptr};
  kb::Status fail_write_{kb::Status::Ok};
  kb::Milliseconds last_timeout_;
  uint32_t baud_{115200U};
  bool ready_{false};
};

static_assert(kb::iface::UartBaudControl<FakeUart> && kb::iface::UartTxDrain<FakeUart>,
              "FakeUart conforms to UartPort with baud control and drain");

} // namespace kb::iface::host

#endif // KB_INTERFACES_HOST_FAKE_UART_HPP
