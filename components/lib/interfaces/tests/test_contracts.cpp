// Conformance of the host fakes and the *Ref values to every kb::iface contract, and the behaviour
// the contracts promise (spans, results, failure kinds), exercised through the type-erased
// references. SPDX-License-Identifier: MIT Copyright (c) 2026 The kb-core authors
#include <array>
#include <cstdint>
#include <span>

#include "kb/interfaces/host/host.hpp"
#include "kb/interfaces/interfaces.hpp"
#include "kb_test.hpp"

namespace h = kb::iface::host;
namespace i = kb::iface;
using kb::Status;

namespace {
// A type with the right names and the wrong shapes satisfies nothing.
struct NotABus {
  int state{0};
  [[nodiscard]] int EnsureReady() const {
    return state;
  }
};
static_assert(!i::I2cBus<NotABus> && !i::SpiDevice<NotABus> && !i::UartPort<NotABus> &&
              !i::CanBus<NotABus>);
static_assert(!i::GpioOutput<int> && !i::GpioInput<int> && !i::AdcChannel<int> &&
              !i::PwmOutput<int>);
static_assert(!i::MonotonicClock<int> && !i::BusyWait<int> && !i::LogSink<int>);
static_assert(!i::SpiClockControl<i::SpiDeviceRef>, "a Ref erases only the base contract");
static_assert(!i::GpioInterrupt<i::GpioInputRef>);
} // namespace

KB_TEST("i2c: register writes and reads, repeated start, failure kinds") {
  h::FakeI2cBus bus{};
  i::I2cBusRef ref{bus};
  const std::array<uint8_t, 2> tx{0x10U, 0xAAU};
  std::array<uint8_t, 1> rx{};
  KB_CHECK_EQ(ref.Write(0x40U, tx), Status::NotReady);
  bus.FailEnsureReady(true);
  KB_CHECK_EQ(ref.EnsureReady(), Status::NotReady);
  bus.FailEnsureReady(false);
  KB_REQUIRE(ref.EnsureReady() == Status::Ok);
  KB_CHECK(bus.AddDevice(0x40U));
  KB_CHECK(!bus.AddDevice(0x40U));
  KB_CHECK(!bus.AddDevice(0x07U));
  KB_CHECK(!bus.AddDevice(0x78U));
  KB_CHECK(bus.AddDevice(0x41U) && bus.AddDevice(0x42U) && bus.AddDevice(0x43U) &&
           !bus.AddDevice(0x44U));
  KB_CHECK_EQ(ref.Write(0x40U, tx), Status::Ok);
  KB_CHECK(ref.LastFailure() == i::I2cFailure::None);
  const std::array<uint8_t, 1> reg{0x10U};
  KB_CHECK_EQ(ref.WriteRead(0x40U, reg, rx), Status::Ok);
  KB_CHECK_EQ(rx[0], 0xAAU);
  KB_CHECK_EQ(ref.Read(0x40U, rx), Status::Ok);
  KB_CHECK_EQ(ref.Write(0x40U, std::span<const uint8_t>{}), Status::Ok);
  KB_CHECK_EQ(ref.Write(0x50U, tx), Status::NoDevice);
  KB_CHECK(ref.LastFailure() == i::I2cFailure::AddressNack);
  KB_CHECK_EQ(ref.WriteRead(0x50U, reg, rx), Status::NoDevice);
  bus.FailNext(i::I2cFailure::DataNack);
  KB_CHECK_EQ(ref.Write(0x40U, tx), Status::Nack);
  bus.FailNext(i::I2cFailure::Timeout, 2U);
  KB_CHECK_EQ(ref.Read(0x40U, rx), Status::Timeout);
  KB_CHECK_EQ(ref.Read(0x40U, rx), Status::Timeout);
  KB_CHECK_EQ(ref.Read(0x40U, rx), Status::Ok);
  bus.FailNext(i::I2cFailure::Bus);
  KB_CHECK_EQ(ref.Read(0x40U, rx), Status::BusError);
  bus.FailNext(i::I2cFailure::BusStuck);
  KB_CHECK_EQ(ref.Read(0x40U, rx), Status::BusError);
  bus.FailNext(i::I2cFailure::None);
  KB_CHECK(bus.Device(0x40U) != nullptr && bus.Device(0x55U) == nullptr && bus.Ready());
  KB_CHECK(bus.Transfers() > 10U);
}

KB_TEST("spi: full duplex, write only, read only, span rules, clock and failures") {
  h::FakeSpiDevice spi{};
  i::SpiDeviceRef ref{spi};
  const std::array<uint8_t, 2> tx{1U, 2U};
  std::array<uint8_t, 2> rx{};
  std::array<uint8_t, 3> rx3{};
  KB_CHECK_EQ(ref.Transfer(tx, rx), Status::NotReady);
  KB_REQUIRE(ref.EnsureReady() == Status::Ok);
  const std::array<uint8_t, 3> answer{7U, 8U, 9U};
  KB_REQUIRE(spi.Respond(answer));
  KB_CHECK_EQ(ref.Transfer(tx, rx), Status::Ok);
  KB_CHECK_EQ(rx[1], 8U);
  KB_CHECK_EQ(ref.Transfer(tx, rx3), Status::InvalidArgument);
  KB_CHECK_EQ(ref.Transfer(tx, std::span<uint8_t>{}), Status::Ok);
  KB_CHECK_EQ(ref.Transfer(std::span<const uint8_t>{}, rx), Status::Ok);
  KB_CHECK_EQ(rx[0], 0U);
  spi.FailNext(Status::Timeout);
  KB_CHECK_EQ(ref.Transfer(tx, rx), Status::Timeout);
  KB_CHECK_EQ(spi.Transfers(), 3U);
  KB_CHECK_EQ(spi.Sent().size(), 6U);
  KB_CHECK_EQ(spi.SetClockHz(0U), Status::InvalidArgument);
  KB_CHECK_EQ(spi.SetClockHz(90000000U), Status::InvalidArgument);
  KB_CHECK_EQ(spi.SetClockHz(4000000U), Status::Ok);
  KB_CHECK_EQ(spi.ClockHz(), 4000000U);
  KB_CHECK(i::SpiSpansValid(std::span<const uint8_t>{}, std::span<uint8_t>{}));
}

KB_TEST("uart: write, partial and timed-out reads, flush, baud and drain") {
  h::FakeUart uart{};
  i::UartPortRef ref{uart};
  const std::array<uint8_t, 3> tx{1U, 2U, 3U};
  std::array<uint8_t, 4> rx{};
  KB_CHECK_EQ(ref.Write(tx), Status::NotReady);
  KB_CHECK_EQ(ref.Read(rx, kb::Milliseconds{1U}).Error(), Status::NotReady);
  KB_CHECK_EQ(uart.WaitTxDone(kb::Milliseconds{1U}), Status::NotReady);
  KB_REQUIRE(ref.EnsureReady() == Status::Ok);
  KB_CHECK_EQ(ref.Write(tx), Status::Ok);
  KB_CHECK_EQ(uart.Written().size(), 3U);
  uart.ClearWritten();
  KB_CHECK(uart.Written().empty());
  KB_REQUIRE(uart.Feed(tx));
  const kb::Result<std::size_t> got = ref.Read(rx, kb::Milliseconds{10U});
  KB_CHECK(got.IsOk() && (got.Value() == 3U));
  KB_CHECK_EQ(ref.Read(rx, kb::Milliseconds{10U}).Error(), Status::Timeout);
  KB_CHECK(ref.Read(std::span<uint8_t>{}, kb::Milliseconds{0U}).IsOk());
  KB_REQUIRE(uart.Feed(tx));
  KB_CHECK_EQ(ref.FlushInput(), Status::Ok);
  KB_CHECK_EQ(ref.Read(rx, kb::Milliseconds{1U}).Error(), Status::Timeout);
  uart.FailNextWrite(Status::BusError);
  KB_CHECK_EQ(ref.Write(tx), Status::BusError);
  KB_CHECK_EQ(uart.SetBaud(0U), Status::InvalidArgument);
  KB_CHECK_EQ(uart.SetBaud(9600U), Status::Ok);
  KB_CHECK_EQ(uart.Baud(), 9600U);
  KB_CHECK_EQ(uart.WaitTxDone(kb::Milliseconds{1U}), Status::Ok);
  std::array<uint8_t, 600> big{};
  KB_CHECK(!uart.Feed(big));
}

KB_TEST("gpio: output and input through Refs, interrupts on edges") {
  h::FakeGpioPin pin{};
  i::GpioOutputRef out{pin};
  i::GpioInputRef in{pin};
  i::Level level = i::Level::High;
  KB_CHECK_EQ(out.Set(i::Level::High), Status::Ok);
  KB_CHECK(in.Get(level) == Status::Ok && level == i::Level::High);
  KB_CHECK_EQ(out.Toggle(), Status::Ok);
  KB_CHECK(in.Get(level) == Status::Ok && level == i::Level::Low);
  KB_CHECK_EQ(out.Toggle(), Status::Ok);
  KB_CHECK_EQ(pin.Writes(), 3U);
  pin.FailNext(Status::BusError);
  KB_CHECK_EQ(out.Set(i::Level::Low), Status::BusError);
  pin.FailNext(Status::BusError);
  KB_CHECK_EQ(in.Get(level), Status::BusError);
  static uint32_t fired = 0U;
  const i::GpioCallback count = [](void* c) noexcept { ++*static_cast<uint32_t*>(c); };
  KB_CHECK_EQ(pin.EnableInterrupt(i::Edge::Rising, nullptr, nullptr), Status::InvalidArgument);
  pin.FailNext(Status::Busy);
  KB_CHECK_EQ(pin.EnableInterrupt(i::Edge::Rising, count, &fired), Status::Busy);
  KB_CHECK_EQ(pin.EnableInterrupt(i::Edge::Rising, count, &fired), Status::Ok);
  pin.Drive(i::Level::Low);
  pin.Drive(i::Level::High);
  pin.Drive(i::Level::High);
  KB_CHECK_EQ(fired, 1U);
  KB_CHECK_EQ(pin.EnableInterrupt(i::Edge::Falling, count, &fired), Status::Ok);
  pin.Drive(i::Level::Low);
  pin.Drive(i::Level::High);
  KB_CHECK_EQ(fired, 2U);
  KB_CHECK_EQ(pin.EnableInterrupt(i::Edge::Both, count, &fired), Status::Ok);
  pin.Drive(i::Level::Low);
  pin.Drive(i::Level::High);
  KB_CHECK_EQ(fired, 4U);
  KB_CHECK_EQ(pin.DisableInterrupt(), Status::Ok);
  pin.Drive(i::Level::Low);
  KB_CHECK_EQ(pin.Interrupts(), 4U);
}

KB_TEST("adc, pwm: conversions, failures, duty clamp and frequency range") {
  h::FakeAdc adc{};
  i::AdcChannelRef ref{adc};
  adc.SetRaw(5000U);
  KB_CHECK_EQ(ref.ReadRaw().Value(), 4095U);
  KB_CHECK_EQ(ref.ReadMillivolts().Value(), 3300);
  adc.SetRaw(0U);
  KB_CHECK_EQ(ref.ReadMillivolts().Value(), 0);
  KB_CHECK_EQ(ref.FullScale(), 4095U);
  adc.FailNext(Status::Timeout);
  KB_CHECK_EQ(ref.ReadRaw().Error(), Status::Timeout);
  adc.FailNext(Status::Timeout);
  KB_CHECK_EQ(ref.ReadMillivolts().Error(), Status::Timeout);
  h::FakePwm pwm{};
  i::PwmOutputRef p{pwm};
  KB_CHECK_EQ(p.SetFrequencyHz(0U), Status::InvalidArgument);
  KB_CHECK_EQ(p.SetFrequencyHz(50000000U), Status::InvalidArgument);
  KB_CHECK_EQ(p.SetFrequencyHz(20000U), Status::Ok);
  KB_CHECK_EQ(p.SetDuty(i::Duty{12000U}), Status::Ok);
  KB_CHECK(pwm.DutyNow() == i::Duty{i::Duty::kFull});
  KB_CHECK_EQ(p.Enable(true), Status::Ok);
  KB_CHECK(pwm.Enabled() && (pwm.FrequencyHz() == 20000U));
  KB_CHECK_EQ(i::Duty{2500U}.Permyriad(), 2500U);
}

KB_TEST("can: send, receive, dlc check, bus-off and queue limits") {
  h::FakeCanBus can{};
  i::CanBusRef ref{can};
  i::CanFrame frame{};
  frame.id = 0x123U;
  frame.dlc = 2U;
  KB_CHECK_EQ(ref.Send(frame, kb::Milliseconds{1U}), Status::NotReady);
  KB_CHECK_EQ(ref.Receive(frame, kb::Milliseconds{1U}), Status::NotReady);
  KB_REQUIRE(ref.EnsureReady() == Status::Ok);
  KB_CHECK_EQ(ref.Send(frame, kb::Milliseconds{1U}), Status::Ok);
  KB_CHECK_EQ(can.Sent().size(), 1U);
  i::CanFrame bad{};
  bad.dlc = 9U;
  KB_CHECK_EQ(ref.Send(bad, kb::Milliseconds{1U}), Status::InvalidArgument);
  can.SetBusOff(true);
  KB_CHECK_EQ(ref.Send(frame, kb::Milliseconds{1U}), Status::BusError);
  can.SetBusOff(false);
  for (uint32_t n = 0U; n < 31U; ++n) {
    KB_CHECK_EQ(ref.Send(frame, kb::Milliseconds{1U}), Status::Ok);
  }
  KB_CHECK_EQ(ref.Send(frame, kb::Milliseconds{1U}), Status::Full);
  i::CanFrame got{};
  KB_CHECK_EQ(ref.Receive(got, kb::Milliseconds{1U}), Status::Timeout);
  frame.id = 0x7FFU;
  KB_REQUIRE(can.Inject(frame));
  KB_CHECK_EQ(ref.Receive(got, kb::Milliseconds{1U}), Status::Ok);
  KB_CHECK_EQ(got.id, 0x7FFU);
}

KB_TEST("time and log: clock jumps near 2^32 us, busy waits, log levels") {
  h::FakeClock clock{};
  i::MonotonicClockRef c{clock};
  clock.Set(kb::Instant{0xFFFFFFF0ULL});
  const kb::Instant before = c.Now();
  h::FakeBusyWait wait{};
  i::BusyWaitRef w{wait};
  wait.Attach(&clock);
  w.WaitUs(kb::Microseconds{0x20U});
  KB_CHECK_EQ(kb::utils::ElapsedUs(c.Now(), before), 0x20U);
  KB_CHECK_EQ(kb::utils::ElapsedSince(kb::utils::NarrowUs(c.Now()), kb::utils::NarrowUs(before)),
              0x20U);
  wait.Attach(nullptr);
  w.WaitUs(kb::Microseconds{5U});
  KB_CHECK(wait.TotalUs() == 0x25U);
  h::FakeLogSink log{};
  i::LogSinkRef l{log};
  l.Write(i::LogLevel::Warning, "low battery");
  l.Write(i::LogLevel::Info, "ready");
  KB_CHECK_EQ(log.Count(i::LogLevel::Warning), 1U);
  KB_CHECK(log.Last() == "ready");
}

// The vocabulary's numbers and defaults are part of the contract a port and a handler share.
static_assert(!i::UartBaudControl<i::UartPortRef> && !i::UartTxDrain<i::UartPortRef>,
              "the optional UART members are required in addition to the base contract");
static_assert(i::CanFrame{}.data.size() == 8U);

KB_TEST("vocabulary: enumerator values, frame and duty defaults, the duty clamp") {
  KB_CHECK_EQ(static_cast<uint8_t>(i::Edge::Both), 2U);
  KB_CHECK_EQ(static_cast<uint8_t>(i::I2cFailure::BusStuck), 5U);
  KB_CHECK_EQ(static_cast<uint8_t>(i::LogLevel::Error), 0U);
  KB_CHECK_EQ(static_cast<uint8_t>(i::LogLevel::Warning), 1U);
  KB_CHECK_EQ(static_cast<uint8_t>(i::LogLevel::Info), 2U);
  KB_CHECK_EQ(static_cast<uint8_t>(i::LogLevel::Debug), 3U);
  const i::CanFrame frame{};
  KB_CHECK((frame.id == 0U) && (!frame.extended) && (!frame.remote) && (frame.dlc == 0U));
  KB_CHECK_EQ(i::kCanMaxDlc, 8U);
  KB_CHECK_EQ(i::Duty{}.Permyriad(), 0U);
  KB_CHECK_EQ(i::Duty{kb_test::Rt(uint16_t{10001U})}.Permyriad(), 10000U);
  KB_CHECK_EQ(i::Duty::kFull, 10000U);
}
