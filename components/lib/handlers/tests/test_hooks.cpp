// The hook bases of kb::handlers over the host fakes and small scripted ports: every I2cHooks,
// SpiHooks, UartHooks, AdcHooks and GpioHooks method with good and null arguments, the counters and
// OnBusFailure, the optional contracts (clock, baud, drain), and HookTiming's delay split and clock
// narrowing.
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "handler_test_support.hpp"
#include "kb/rtos/time.hpp"
#include "kb_test.hpp"

using kb::Milliseconds;
using kb::Status;
namespace h = kb::handlers;
namespace host = kb::iface::host;

namespace {

/// A clock and a spin wait that advances it, and the HookTiming over both.
struct TimingRig {
  host::FakeClock clock{};
  host::FakeBusyWait spin{};
  h::HookTiming timing;
  TimingRig() noexcept : timing(kb::iface::MonotonicClockRef{clock}, kb::iface::BusyWaitRef{spin}) {
    spin.Attach(&clock);
  }
};

/// Counts the failures the hook base reports to its port.
struct FailureLog {
  uint32_t seen{0U};
  Status last{Status::Ok};
};

template <typename Bus>
class I2cPort : public h::I2cHooks<I2cPort<Bus>, Bus> {
public:
  I2cPort(Bus& bus, const h::HookTiming& timing) noexcept
      : h::I2cHooks<I2cPort<Bus>, Bus>(bus, timing) {}
  void OnBusFailure(Status status) noexcept {
    ++log.seen;
    log.last = status;
  }
  FailureLog log{};
};

template <typename Bus>
class QuietI2cPort : public h::I2cHooks<QuietI2cPort<Bus>, Bus> {
public:
  QuietI2cPort(Bus& bus, const h::HookTiming& timing) noexcept
      : h::I2cHooks<QuietI2cPort<Bus>, Bus>(bus, timing) {}
};

template <typename Dev>
class SpiPort : public h::SpiHooks<SpiPort<Dev>, Dev> {
public:
  SpiPort(Dev& dev, const h::HookTiming& timing) noexcept
      : h::SpiHooks<SpiPort<Dev>, Dev>(dev, timing) {}
  void OnBusFailure(Status status) noexcept {
    ++log.seen;
    log.last = status;
  }
  FailureLog log{};
};

template <typename Port>
class UartPort : public h::UartHooks<UartPort<Port>, Port> {
public:
  UartPort(Port& port, const h::HookTiming& timing) noexcept
      : h::UartHooks<UartPort<Port>, Port>(port, timing) {}
  void OnBusFailure(Status status) noexcept {
    ++log.seen;
    log.last = status;
  }
  FailureLog log{};
};

template <typename Adc>
class AdcPort : public h::AdcHooks<AdcPort<Adc>, Adc> {
public:
  AdcPort(Adc& adc, const h::HookTiming& timing) noexcept
      : h::AdcHooks<AdcPort<Adc>, Adc>(adc, timing) {}
};

class PinPort : public h::GpioHooks<PinPort, 3U, 3U> {
public:
  PinPort(const std::array<h::OutputPin, 3U>& outputs,
          const std::array<h::InputPin, 3U>& inputs) noexcept
      : h::GpioHooks<PinPort, 3U, 3U>(outputs, inputs) {}
  void OnBusFailure(Status status) noexcept {
    ++log.seen;
    log.last = status;
  }
  FailureLog log{};
};

/// One answer of ScriptedUart::Read.
struct ReadStep {
  Status status{Status::Timeout};
  uint32_t bytes{0U};
  uint64_t advance_us{1000U};
};

/// A UART port (no baud control, no drain) whose reads follow a script and let the fake clock run.
class ScriptedUart {
public:
  explicit ScriptedUart(host::FakeClock& clock) noexcept : clock_(clock) {}

  void Push(const ReadStep& step) noexcept {
    if (count_ < steps_.size()) {
      steps_[count_] = step;
      ++count_;
    }
  }

  [[nodiscard]] static kb::Status EnsureReady() noexcept {
    return Status::Ok;
  }
  [[nodiscard]] kb::Status Write(std::span<const uint8_t> tx) noexcept {
    written_ += static_cast<uint32_t>(tx.size());
    return write_status_;
  }
  [[nodiscard]] kb::Result<std::size_t> Read(std::span<uint8_t> rx, Milliseconds timeout) noexcept {
    ++reads_;
    last_timeout_ = timeout;
    ReadStep step{};
    if (next_ < count_) {
      step = steps_[next_];
      ++next_;
    }
    clock_.Advance(step.advance_us);
    const std::size_t n = (step.bytes < rx.size()) ? step.bytes : rx.size();
    for (std::size_t i = 0U; i < n; ++i) {
      rx[i] = static_cast<uint8_t>(0xA0U + i);
    }
    return (step.status == Status::Ok) ? kb::Result<std::size_t>::Success(n)
                                       : kb::Result<std::size_t>::Failure(step.status);
  }
  [[nodiscard]] kb::Status FlushInput() noexcept {
    ++flushes_;
    return flush_status_;
  }

  /// What the next writes answer.
  void SetWriteStatus(Status status) noexcept {
    write_status_ = status;
  }
  /// What the next input flushes answer.
  void SetFlushStatus(Status status) noexcept {
    flush_status_ = status;
  }
  [[nodiscard]] Milliseconds LastTimeout() const noexcept {
    return last_timeout_;
  }
  [[nodiscard]] uint32_t Reads() const noexcept {
    return reads_;
  }
  [[nodiscard]] uint32_t Flushes() const noexcept {
    return flushes_;
  }
  [[nodiscard]] uint32_t Written() const noexcept {
    return written_;
  }

private:
  Status write_status_{Status::Ok};
  Status flush_status_{Status::Ok};
  Milliseconds last_timeout_;
  uint32_t reads_{0U};
  uint32_t flushes_{0U};
  uint32_t written_{0U};
  host::FakeClock& clock_;
  std::array<ReadStep, 8> steps_{};
  std::size_t count_{0U};
  std::size_t next_{0U};
};

static_assert(kb::iface::UartPort<ScriptedUart>);
static_assert(!kb::iface::UartBaudControl<ScriptedUart>);
static_assert(!kb::iface::UartTxDrain<ScriptedUart>);

} // namespace

// --- HookTiming --------------------------------------------------------------------------------

KB_TEST("hooks: DelayUs spins below 1 ms and sleeps from 1 ms; DelayMs sleeps") {
  TimingRig rig{};
  const kb::Instant before = kb::rtos::Now();
  rig.timing.DelayUs(999U);
  KB_CHECK_EQ(rig.spin.TotalUs(), 999U);
  KB_CHECK_EQ(rig.timing.Now().Us(), 999U);
  KB_CHECK(kb::rtos::Now() == before); // no sleep
  rig.timing.DelayUs(0U);
  KB_CHECK_EQ(rig.spin.TotalUs(), 999U);
  rig.timing.DelayUs(1000U);
  KB_CHECK_EQ(rig.spin.TotalUs(), 999U); // no spin
  KB_CHECK(kb::utils::ElapsedUs(kb::rtos::Now(), before) >= 1000U);
  const kb::Instant mid = kb::rtos::Now();
  rig.timing.DelayUs(2500U);
  KB_CHECK(kb::utils::ElapsedUs(kb::rtos::Now(), mid) >= 2500U);
  const kb::Instant late = kb::rtos::Now();
  h::HookTiming::DelayMs(3U);
  KB_CHECK(kb::utils::ElapsedUs(kb::rtos::Now(), late) >= 3000U);
  const kb::Instant last = kb::rtos::Now();
  h::HookTiming::DelayMs(0U);
  KB_CHECK(kb::rtos::Now() == last);
}

KB_TEST("hooks: the 32-bit clocks are the low bits of the 64-bit clock and wrap") {
  TimingRig rig{};
  rig.clock.Set(kb::Instant{(uint64_t{1U} << 32U) + 5U});
  KB_CHECK_EQ(rig.timing.NowUs32(), 5U);
  KB_CHECK_EQ(rig.timing.Now().Us(), (uint64_t{1U} << 32U) + 5U);
  rig.clock.Set(kb::Instant{((uint64_t{1U} << 32U) + 7U) * 1000U});
  KB_CHECK_EQ(rig.timing.NowMs32(), 7U);
  rig.clock.Set(kb::Instant{0xFFFFFFFFULL});
  KB_CHECK_EQ(rig.timing.NowUs32(), 0xFFFFFFFFU);
  rig.clock.Advance(1U);
  KB_CHECK_EQ(rig.timing.NowUs32(), 0U); // wrapped
  KB_CHECK_EQ(h::kSpinBelowUs, 1000U);
}

// --- I2cHooks ----------------------------------------------------------------------------------

KB_TEST("hooks: I2C transfers, null arguments and failure counting") {
  TimingRig rig{};
  host::FakeI2cBus bus{};
  KB_REQUIRE(bus.AddDevice(0x20U));
  I2cPort<host::FakeI2cBus> port{bus, rig.timing};
  KB_CHECK(port.BusReady());
  const std::array<uint8_t, 3> tx{0x10U, 0xABU, 0xCDU};
  KB_CHECK(port.BusWrite(0x20U, tx.data(), tx.size()));
  std::array<uint8_t, 2> rx{};
  const uint8_t reg = 0x10U;
  KB_CHECK(port.BusWriteRead(0x20U, &reg, 1U, rx.data(), rx.size()));
  KB_CHECK_EQ(rx[0], 0xABU);
  KB_CHECK_EQ(rx[1], 0xCDU);
  KB_CHECK(port.BusRead(0x20U, rx.data(), 1U));
  KB_CHECK(port.BusWrite(0x20U, nullptr, 0U)); // an empty write is allowed
  KB_CHECK(port.BusRead(0x20U, nullptr, 0U));  // an empty read too
  KB_CHECK(port.BusWriteRead(0x20U, nullptr, 0U, rx.data(), 1U));
  KB_CHECK(port.BusWriteRead(0x20U, &reg, 1U, nullptr, 0U));
  KB_CHECK_EQ(port.Counters().transfers, 8U);
  KB_CHECK_EQ(port.Counters().failures, 0U);
  KB_CHECK_EQ(port.log.seen, 0U);

  KB_CHECK(!port.BusWrite(0x20U, nullptr, 2U));
  KB_CHECK(!port.BusRead(0x20U, nullptr, 2U));
  KB_CHECK(!port.BusWriteRead(0x20U, nullptr, 1U, rx.data(), 1U));
  KB_CHECK(!port.BusWriteRead(0x20U, &reg, 1U, nullptr, 1U));
  KB_CHECK_EQ(port.Counters().failures, 4U);
  KB_CHECK_EQ(port.Counters().last_status, Status::InvalidArgument);
  KB_CHECK_EQ(port.log.seen, 4U);
  KB_CHECK_EQ(bus.Transfers(), 7U); // refused calls reach no bus

  bus.FailNext(kb::iface::I2cFailure::AddressNack);
  KB_CHECK(!port.BusRead(0x20U, rx.data(), 1U));
  KB_CHECK_EQ(port.BusLastFailure(), kb::iface::I2cFailure::AddressNack);
  KB_CHECK_EQ(port.Counters().last_status, Status::NoDevice);
  KB_CHECK_EQ(port.log.last, Status::NoDevice);
  bus.FailEnsureReady(true);
  KB_CHECK(!port.BusReady());
  KB_CHECK_EQ(port.Counters().failures, 6U);
  KB_CHECK_EQ(port.Counters().transfers, 14U);
  port.Timing().DelayUs(10U);
  KB_CHECK_EQ(rig.spin.TotalUs(), 10U);
}

KB_TEST("hooks: I2C over the type-erased bus, without OnBusFailure") {
  TimingRig rig{};
  host::FakeI2cBus bus{};
  KB_REQUIRE(bus.AddDevice(0x21U));
  kb::iface::I2cBusRef ref{bus};
  QuietI2cPort<kb::iface::I2cBusRef> port{ref, rig.timing};
  KB_CHECK(port.BusReady());
  KB_CHECK(!port.BusWrite(0x22U, nullptr, 0U)); // nobody at 0x22
  KB_CHECK_EQ(port.Counters().last_status, Status::NoDevice);
  KB_CHECK_EQ(port.BusLastFailure(), kb::iface::I2cFailure::AddressNack);
  KB_CHECK_EQ(port.Counters().failures, 1U);
}

// --- SpiHooks ----------------------------------------------------------------------------------

KB_TEST(
    "hooks: SPI frames with tx, rx, both or neither; clock control with and without the contract") {
  TimingRig rig{};
  host::FakeSpiDevice dev{};
  SpiPort<host::FakeSpiDevice> port{dev, rig.timing};
  KB_CHECK(port.BusReady());
  const std::array<uint8_t, 2> answer{0x5AU, 0xA5U};
  KB_REQUIRE(dev.Respond(answer));
  const std::array<uint8_t, 2> tx{0x01U, 0x02U};
  std::array<uint8_t, 2> rx{};
  KB_CHECK(port.BusTransfer(tx.data(), rx.data(), 2U));
  KB_CHECK_EQ(rx[0], 0x5AU);
  KB_CHECK_EQ(rx[1], 0xA5U);
  KB_CHECK(port.BusTransfer(tx.data(), nullptr, 2U)); // write only
  KB_CHECK(port.BusTransfer(nullptr, rx.data(), 2U)); // read only
  KB_CHECK(port.BusTransfer(nullptr, nullptr, 0U));   // an empty frame
  KB_CHECK_EQ(dev.Sent().size(), 6U);
  KB_CHECK(!port.BusTransfer(nullptr, nullptr, 3U)); // nothing to send or receive
  KB_CHECK_EQ(port.Counters().last_status, Status::InvalidArgument);
  KB_CHECK_EQ(port.log.seen, 1U);
  dev.FailNext(Status::BusError);
  KB_CHECK(!port.BusTransfer(tx.data(), rx.data(), 2U));
  KB_CHECK_EQ(port.log.last, Status::BusError);

  KB_CHECK(port.BusSetClock(4000000U));
  KB_CHECK_EQ(dev.ClockHz(), 4000000U);
  KB_CHECK(!port.BusSetClock(0U));
  KB_CHECK_EQ(port.Counters().last_status, Status::InvalidArgument);

  kb::iface::SpiDeviceRef ref{dev};
  SpiPort<kb::iface::SpiDeviceRef> plain{ref, rig.timing};
  KB_CHECK(!plain.BusSetClock(1000000U));
  KB_CHECK_EQ(plain.Counters().last_status, Status::Unsupported);
  KB_CHECK_EQ(plain.log.seen, 1U);
  KB_CHECK_EQ(port.Counters().transfers, 9U);
  plain.Timing().DelayUs(7U);
  KB_CHECK_EQ(rig.spin.TotalUs(), 7U);
}

// --- UartHooks ---------------------------------------------------------------------------------

KB_TEST("hooks: UART BusRead refuses a null buffer and does nothing for zero bytes") {
  TimingRig rig{};
  ScriptedUart uart{rig.clock};
  UartPort<ScriptedUart> port{uart, rig.timing};
  KB_CHECK_EQ(port.BusRead(nullptr, 3U, 10U), 0U);
  KB_CHECK_EQ(port.Counters().last_status, Status::BusError);
  KB_CHECK_EQ(port.log.seen, 1U);
  KB_CHECK_EQ(port.BusRead(nullptr, 0U, 10U), 0U);
  std::array<uint8_t, 2> rx{};
  KB_CHECK_EQ(port.BusRead(rx.data(), 0U, 10U), 0U);
  KB_CHECK_EQ(uart.Reads(), 0U);
  KB_CHECK_EQ(port.Counters().failures, 1U);
  KB_CHECK_EQ(port.Counters().transfers, 3U);
}

KB_TEST("hooks: UART BusRead collects partial reads within the timeout") {
  TimingRig rig{};
  ScriptedUart uart{rig.clock};
  UartPort<ScriptedUart> port{uart, rig.timing};
  std::array<uint8_t, 4> rx{};
  uart.Push(ReadStep{.status = Status::Ok, .bytes = 4U, .advance_us = 0U});
  KB_CHECK_EQ(port.BusRead(rx.data(), rx.size(), 10U), 4U);
  KB_CHECK_EQ(uart.Reads(), 1U);
  KB_CHECK(uart.LastTimeout() == Milliseconds{10U});
  KB_CHECK_EQ(rx[3], 0xA3U);

  ScriptedUart split{rig.clock};
  UartPort<ScriptedUart> port2{split, rig.timing};
  split.Push(ReadStep{.status = Status::Ok, .bytes = 2U, .advance_us = 1500U});
  split.Push(ReadStep{.status = Status::Ok, .bytes = 2U, .advance_us = 0U});
  rx = {};
  KB_CHECK_EQ(port2.BusRead(rx.data(), rx.size(), 10U), 4U);
  KB_CHECK_EQ(split.Reads(), 2U);
  KB_CHECK(split.LastTimeout() == Milliseconds{9U}); // 8.5 ms left, rounded up
  KB_CHECK_EQ(rx[2], 0xA0U);
  KB_CHECK_EQ(port2.Counters().failures, 0U);
}

KB_TEST("hooks: UART BusRead returns what arrived on a timeout without counting a failure") {
  TimingRig rig{};
  ScriptedUart uart{rig.clock};
  UartPort<ScriptedUart> port{uart, rig.timing};
  std::array<uint8_t, 4> rx{};
  uart.Push(
      ReadStep{.status = Status::Ok, .bytes = 1U, .advance_us = 0U}); // then timeouts of 1 ms each
  KB_CHECK_EQ(port.BusRead(rx.data(), rx.size(), 3U), 1U);
  KB_CHECK_EQ(uart.Reads(),
              5U); // 1 byte, then 3 ms of timeouts, then one last try with nothing left
  KB_CHECK(uart.LastTimeout() == Milliseconds{0U});
  KB_CHECK_EQ(port.Counters().failures, 0U);
  KB_CHECK_EQ(port.Counters().transfers, 1U);
}

KB_TEST("hooks: UART BusRead stops at a port failure and counts it") {
  TimingRig rig{};
  ScriptedUart uart{rig.clock};
  UartPort<ScriptedUart> port{uart, rig.timing};
  std::array<uint8_t, 4> rx{};
  uart.Push(ReadStep{.status = Status::Ok, .bytes = 1U, .advance_us = 0U});
  uart.Push(ReadStep{.status = Status::BusError, .bytes = 0U, .advance_us = 0U});
  KB_CHECK_EQ(port.BusRead(rx.data(), rx.size(), 50U), 1U);
  KB_CHECK_EQ(uart.Reads(), 2U);
  KB_CHECK_EQ(port.Counters().failures, 1U);
  KB_CHECK_EQ(port.log.last, Status::BusError);
}

KB_TEST("hooks: UART BusRead clips a timeout beyond 32 bits of microseconds") {
  TimingRig rig{};
  ScriptedUart uart{rig.clock};
  UartPort<ScriptedUart> port{uart, rig.timing};
  std::array<uint8_t, 2> rx{};
  uart.Push(ReadStep{.status = Status::Ok, .bytes = 2U, .advance_us = 0U});
  KB_CHECK_EQ(port.BusRead(rx.data(), rx.size(), 0xFFFFFFFFU), 2U);
  KB_CHECK(uart.LastTimeout() == Milliseconds{4294968U}); // 0xFFFFFFFF us, rounded up to ms
}

KB_TEST("hooks: UART BusReadSome takes what arrived; a timeout is not a failure") {
  TimingRig rig{};
  ScriptedUart uart{rig.clock};
  UartPort<ScriptedUart> port{uart, rig.timing};
  std::array<uint8_t, 8> rx{};
  KB_CHECK_EQ(port.BusReadSome(nullptr, 2U, 5U), 0U);
  KB_CHECK_EQ(port.Counters().last_status, Status::InvalidArgument);
  KB_CHECK_EQ(port.BusReadSome(nullptr, 0U, 5U), 0U); // nothing asked: a read of no bytes
  uart.Push(ReadStep{.status = Status::Ok, .bytes = 3U, .advance_us = 0U});
  uart.Push(ReadStep{.status = Status::Timeout, .bytes = 0U, .advance_us = 0U});
  uart.Push(ReadStep{.status = Status::BusError, .bytes = 0U, .advance_us = 0U});
  KB_CHECK_EQ(port.BusReadSome(rx.data(), rx.size(), 5U), 3U);
  KB_CHECK(uart.LastTimeout() == Milliseconds{5U});
  KB_CHECK_EQ(port.BusReadSome(rx.data(), rx.size(), 5U), 0U);
  KB_CHECK_EQ(port.Counters().failures, 1U);
  KB_CHECK_EQ(port.BusReadSome(rx.data(), rx.size(), 5U), 0U);
  KB_CHECK_EQ(port.Counters().failures, 2U);
  KB_CHECK_EQ(port.log.last, Status::BusError);
}

KB_TEST("hooks: UART BusExchange flushes, writes, then reads exactly the reply") {
  TimingRig rig{};
  const std::array<uint8_t, 2> tx{0x01U, 0x02U};
  std::array<uint8_t, 3> rx{};
  {
    ScriptedUart uart{rig.clock};
    UartPort<ScriptedUart> port{uart, rig.timing};
    uart.Push(ReadStep{.status = Status::Ok, .bytes = 3U, .advance_us = 0U});
    KB_CHECK(port.BusExchange(tx.data(), tx.size(), rx.data(), rx.size(), 10U));
    KB_CHECK_EQ(uart.Flushes(), 1U);
    KB_CHECK_EQ(uart.Written(), 2U);
  }
  {
    ScriptedUart uart{rig.clock};
    uart.SetFlushStatus(Status::BusError);
    UartPort<ScriptedUart> port{uart, rig.timing};
    KB_CHECK(!port.BusExchange(tx.data(), tx.size(), rx.data(), rx.size(), 10U));
    KB_CHECK_EQ(uart.Written(), 0U);
    KB_CHECK_EQ(uart.Reads(), 0U);
  }
  {
    ScriptedUart uart{rig.clock};
    uart.SetWriteStatus(Status::Timeout);
    UartPort<ScriptedUart> port{uart, rig.timing};
    KB_CHECK(!port.BusExchange(tx.data(), tx.size(), rx.data(), rx.size(), 10U));
    KB_CHECK_EQ(uart.Reads(), 0U);
    KB_CHECK_EQ(port.log.last, Status::Timeout);
  }
  {
    ScriptedUart uart{rig.clock};
    UartPort<ScriptedUart> port{uart, rig.timing};
    uart.Push(ReadStep{
        .status = Status::Ok, .bytes = 2U, .advance_us = 0U}); // one byte short, then timeouts
    KB_CHECK(!port.BusExchange(tx.data(), tx.size(), rx.data(), rx.size(), 2U));
    KB_CHECK_EQ(port.Counters().failures, 0U);
  }
  {
    ScriptedUart uart{rig.clock};
    UartPort<ScriptedUart> port{uart, rig.timing};
    KB_CHECK(!port.BusWrite(nullptr, 1U));
    KB_CHECK(port.BusWrite(nullptr, 0U));
    KB_CHECK(port.BusFlush());
    KB_CHECK(port.BusReady());
  }
}

KB_TEST("hooks: UART baud and drain with and without the optional contracts") {
  TimingRig rig{};
  host::FakeUart uart{};
  UartPort<host::FakeUart> port{uart, rig.timing};
  KB_CHECK(!port.BusWaitTxDone(5U)); // not ready yet
  KB_CHECK_EQ(port.Counters().last_status, Status::NotReady);
  KB_CHECK(port.BusReady());
  KB_CHECK(port.BusWaitTxDone(5U));
  KB_CHECK(port.BusSetBaud(9600U));
  KB_CHECK_EQ(uart.Baud(), 9600U);
  KB_CHECK(!port.BusSetBaud(0U));
  KB_CHECK_EQ(port.Counters().last_status, Status::InvalidArgument);

  kb::iface::UartPortRef ref{uart};
  UartPort<kb::iface::UartPortRef> plain{ref, rig.timing};
  KB_CHECK(!plain.BusSetBaud(9600U));
  KB_CHECK_EQ(plain.Counters().last_status, Status::Unsupported);
  KB_CHECK(plain.BusWaitTxDone(5U)); // no drain contract: Ok
  KB_CHECK_EQ(plain.Counters().failures, 1U);
  plain.Timing().DelayUs(3U);
  KB_CHECK_EQ(rig.spin.TotalUs(), 3U);
}

// --- AdcHooks ----------------------------------------------------------------------------------

KB_TEST("hooks: ADC raw and millivolt conversions, failures give 0") {
  TimingRig rig{};
  host::FakeAdc adc{};
  adc.SetRaw(4095U);
  AdcPort<host::FakeAdc> port{adc, rig.timing};
  uint32_t raw = 1U;
  int32_t mv = 1;
  KB_CHECK(port.AdcRaw(raw));
  KB_CHECK_EQ(raw, 4095U);
  KB_CHECK(port.AdcMillivolts(mv));
  KB_CHECK_EQ(mv, 3300);
  adc.FailNext(Status::Timeout);
  KB_CHECK(!port.AdcRaw(raw));
  KB_CHECK_EQ(raw, 0U);
  adc.FailNext(Status::BusError);
  KB_CHECK(!port.AdcMillivolts(mv));
  KB_CHECK_EQ(mv, 0);
  KB_CHECK_EQ(port.AdcFullScale(), 4095U);
  KB_CHECK_EQ(port.Counters().failures, 2U);
  KB_CHECK_EQ(port.Counters().last_status, Status::BusError);

  kb::iface::AdcChannelRef ref{adc};
  AdcPort<kb::iface::AdcChannelRef> erased{ref, rig.timing};
  KB_CHECK(erased.AdcRaw(raw));
  KB_CHECK_EQ(erased.AdcFullScale(), 4095U);
  erased.Timing().DelayUs(1U);
  KB_CHECK_EQ(rig.spin.TotalUs(), 1U);
}

// --- GpioHooks ---------------------------------------------------------------------------------

KB_TEST("hooks: control outputs with polarity, absent pins and failures") {
  host::FakeGpioPin high_pin{};
  host::FakeGpioPin low_pin{};
  std::array<h::OutputPin, 3U> outputs{};
  outputs[0].pin.emplace(high_pin);
  outputs[1].pin.emplace(low_pin);
  outputs[1].active_low = true;
  const std::array<h::InputPin, 3U> inputs{};
  PinPort port{outputs, inputs};
  KB_CHECK(port.HasOutput(0U) && port.HasOutput(1U));
  KB_CHECK(!port.HasOutput(2U)); // not wired
  KB_CHECK(!port.HasOutput(3U)); // out of range
  KB_CHECK(port.DriveOutput(0U, true));
  KB_CHECK_EQ(high_pin.LevelNow(), kb::iface::Level::High);
  KB_CHECK(port.DriveOutput(0U, false));
  KB_CHECK_EQ(high_pin.LevelNow(), kb::iface::Level::Low);
  KB_CHECK(port.DriveOutput(1U, true));
  KB_CHECK_EQ(low_pin.LevelNow(), kb::iface::Level::Low);
  KB_CHECK(port.DriveOutput(1U, false));
  KB_CHECK_EQ(low_pin.LevelNow(), kb::iface::Level::High);
  KB_CHECK(!port.DriveOutput(2U, true));
  KB_CHECK_EQ(port.Counters().last_status, Status::Unsupported);
  KB_CHECK(!port.DriveOutput(7U, true));
  high_pin.FailNext(Status::BusError);
  KB_CHECK(!port.DriveOutput(0U, true));
  KB_CHECK_EQ(port.log.last, Status::BusError);
  KB_CHECK_EQ(port.Counters().failures, 3U);
  KB_CHECK_EQ(port.Counters().transfers, 7U);
}

KB_TEST("hooks: control inputs with polarity, absent pins and failures") {
  host::FakeGpioPin high_pin{};
  host::FakeGpioPin low_pin{};
  const std::array<h::OutputPin, 3U> outputs{};
  std::array<h::InputPin, 3U> inputs{};
  inputs[0].pin.emplace(high_pin);
  inputs[1].pin.emplace(low_pin);
  inputs[1].active_low = true;
  PinPort port{outputs, inputs};
  KB_CHECK(port.HasInput(0U) && port.HasInput(1U));
  KB_CHECK(!port.HasInput(2U));
  KB_CHECK(!port.HasInput(3U));
  bool asserted = false;
  high_pin.Drive(kb::iface::Level::High);
  KB_CHECK(port.ReadInput(0U, asserted));
  KB_CHECK(asserted);
  high_pin.Drive(kb::iface::Level::Low);
  KB_CHECK(port.ReadInput(0U, asserted));
  KB_CHECK(!asserted);
  low_pin.Drive(kb::iface::Level::Low);
  KB_CHECK(port.ReadInput(1U, asserted));
  KB_CHECK(asserted);
  low_pin.Drive(kb::iface::Level::High);
  KB_CHECK(port.ReadInput(1U, asserted));
  KB_CHECK(!asserted);
  asserted = true;
  KB_CHECK(!port.ReadInput(2U, asserted));
  KB_CHECK(!asserted);
  low_pin.Drive(kb::iface::Level::Low);
  low_pin.FailNext(Status::Timeout);
  asserted = true;
  KB_CHECK(!port.ReadInput(1U, asserted));
  KB_CHECK(!asserted); // a failed read is never "asserted", whatever the polarity
  KB_CHECK_EQ(port.log.last, Status::Timeout);
  KB_CHECK_EQ(port.Counters().failures, 2U);
}
