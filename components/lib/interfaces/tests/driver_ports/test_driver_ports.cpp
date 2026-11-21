// Proof that each kb::iface contract satisfies the drivers' own port concepts through a thin
// adapter: with the host fakes and with the type-erased *Ref values, at compile time (the concepts)
// and at run time (the hooks reach the contract). SPDX-License-Identifier: MIT Copyright (c) 2026
// The kb-core authors
#include <array>
#include <cstdint>

#include "driver_adapters.hpp"
#include "kb/interfaces/host/host.hpp"
#include "kb_test.hpp"

namespace h = kb::iface::host;
namespace i = kb::iface;
using adapters::Bno08xI2c;
using adapters::Bno08xSpi;
using adapters::Pcal9555I2c;
using adapters::Tmc5160Spi;
using adapters::Tmc5160Uart;

// Compile-time: every adapter satisfies its driver's concept over a host fake and over a *Ref.
static_assert(tmc5160::SpiPort<Tmc5160Spi<h::FakeSpiDevice>>);
static_assert(tmc5160::SpiPort<Tmc5160Spi<i::SpiDeviceRef>>);
static_assert(tmc5160::UartPort<Tmc5160Uart<h::FakeUart>>);
static_assert(tmc5160::UartPort<Tmc5160Uart<i::UartPortRef>>);
static_assert(bno08x::HubPort<Bno08xI2c<h::FakeI2cBus, h::FakeGpioPin, h::FakeClock>>);
static_assert(bno08x::HubPort<Bno08xI2c<i::I2cBusRef, i::GpioInputRef, i::MonotonicClockRef>>);
static_assert(
    bno08x::HubPort<Bno08xSpi<h::FakeSpiDevice, h::FakeGpioPin, h::FakeGpioPin, h::FakeClock>>);
static_assert(bno08x::HubPort<
              Bno08xSpi<i::SpiDeviceRef, i::GpioOutputRef, i::GpioInputRef, i::MonotonicClockRef>>);
static_assert(pcal9555::I2cPort<Pcal9555I2c<h::FakeI2cBus, h::FakeLogSink>>);
static_assert(pcal9555::I2cPort<Pcal9555I2c<i::I2cBusRef, i::LogSinkRef>>);
static_assert(pcal9555::detail::HasGetLastFailure<Pcal9555I2c<i::I2cBusRef, i::LogSinkRef>>);
static_assert(pcal9555::detail::HasLog<Pcal9555I2c<i::I2cBusRef, i::LogSinkRef>>);

KB_TEST("driver ports: tmc5160 SPI datagram through a SpiDeviceRef") {
  h::FakeSpiDevice spi{};
  i::SpiDeviceRef ref{spi};
  Tmc5160Spi<i::SpiDeviceRef> port{ref};
  KB_CHECK(!port.Transfer(nullptr, nullptr, 0U)); // not ready yet
  KB_REQUIRE(port.EnsureInitialized());
  const std::array<uint8_t, 5> answer{0x0FU, 0x11U, 0x22U, 0x33U, 0x44U};
  KB_REQUIRE(spi.Respond(answer));
  const std::array<uint8_t, 5> tx{0x00U, 0U, 0U, 0U, 0U};
  std::array<uint8_t, 5> rx{};
  KB_CHECK(port.Transfer(tx.data(), rx.data(), rx.size()));
  KB_CHECK_EQ(rx[4], 0x44U);
  KB_CHECK_EQ(spi.Sent().size(), 5U);
}

KB_TEST("driver ports: tmc5160 single-wire UART exchange, reply and timeout") {
  h::FakeUart uart{};
  Tmc5160Uart<h::FakeUart> port{uart};
  KB_REQUIRE(port.EnsureInitialized());
  uart.OnWrite(
      [](h::FakeUart& u, std::span<const uint8_t> written, void*) noexcept {
        const std::array<uint8_t, 2> reply{0x05U, written[0]};
        static_cast<void>(u.Feed(reply));
      },
      nullptr);
  const std::array<uint8_t, 4> request{0xA5U, 0x00U, 0x01U, 0x02U};
  std::array<uint8_t, 2> reply{};
  KB_CHECK(port.Exchange(request.data(), request.size(), reply.data(), reply.size(), 5U) ==
           tmc5160::BusStatus::Ok);
  KB_CHECK_EQ(reply[1], 0xA5U);
  KB_CHECK_EQ(uart.LastTimeout().Count(), 5U);
  uart.OnWrite(nullptr, nullptr);
  KB_CHECK(port.Exchange(request.data(), request.size(), reply.data(), reply.size(), 5U) ==
           tmc5160::BusStatus::Timeout);
  uart.FailNextWrite(kb::Status::BusError);
  KB_CHECK(port.Exchange(request.data(), request.size(), reply.data(), reply.size(), 5U) ==
           tmc5160::BusStatus::Fault);
}

KB_TEST("driver ports: bno08x over I2C reads, writes, its interrupt line and a narrowed clock") {
  h::FakeI2cBus bus{};
  h::FakeGpioPin int_pin{};
  h::FakeClock clock{};
  KB_REQUIRE(bus.AddDevice(bno08x::i2c::kI2cAddress));
  i::I2cBusRef bus_ref{bus};
  i::GpioInputRef int_ref{int_pin};
  i::MonotonicClockRef clock_ref{clock};
  Bno08xI2c<i::I2cBusRef, i::GpioInputRef, i::MonotonicClockRef> port{bus_ref, int_ref, clock_ref};
  KB_REQUIRE(port.EnsureInitialized());
  const std::array<uint8_t, 3> packet{0x00U, 0xABU, 0xCDU};
  KB_CHECK(port.I2cWrite(bno08x::i2c::kI2cAddress, packet.data(), packet.size()));
  KB_CHECK(!port.I2cWrite(0x4BU, packet.data(), packet.size()));
  std::array<uint8_t, 2> got{};
  KB_CHECK(port.I2cRead(bno08x::i2c::kI2cAddress, got.data(), got.size()));
  int_pin.Drive(i::Level::High);
  KB_CHECK(!port.IntAsserted());
  int_pin.Drive(i::Level::Low);
  KB_CHECK(port.IntAsserted());
  clock.Set(kb::Instant{0x1'0000'0010ULL});
  KB_CHECK_EQ(port.NowUs(), 0x10U);
}

KB_TEST("driver ports: bno08x over SPI with WAKE and H_INTN") {
  h::FakeSpiDevice spi{};
  h::FakeGpioPin wake{};
  h::FakeGpioPin int_pin{};
  h::FakeClock clock{};
  Bno08xSpi<h::FakeSpiDevice, h::FakeGpioPin, h::FakeGpioPin, h::FakeClock> port{spi, wake, int_pin,
                                                                                 clock};
  KB_REQUIRE(port.EnsureInitialized());
  KB_CHECK(port.SetWake(true));
  KB_CHECK(wake.LevelNow() == i::Level::Low);
  KB_CHECK(port.SetWake(false));
  KB_CHECK(wake.LevelNow() == i::Level::High);
  std::array<uint8_t, 4> tx{1U, 2U, 3U, 4U};
  std::array<uint8_t, 4> rx{};
  KB_CHECK(port.SpiTransfer(tx.data(), rx.data(), tx.size()));
  int_pin.FailNext(kb::Status::BusError);
  KB_CHECK(!port.IntAsserted());
  KB_CHECK_EQ(port.NowUs(), 0U);
}

KB_TEST("driver ports: pcal9555 register access, failure kinds and log through Refs") {
  h::FakeI2cBus bus{};
  h::FakeLogSink log{};
  KB_REQUIRE(bus.AddDevice(pcal9555::kI2cAddress));
  i::I2cBusRef bus_ref{bus};
  i::LogSinkRef log_ref{log};
  Pcal9555I2c<i::I2cBusRef, i::LogSinkRef> port{bus_ref, log_ref};
  KB_REQUIRE(port.EnsureInitialized());
  const std::array<uint8_t, 3> write{0x06U, 0xF0U, 0x0FU};
  KB_CHECK(port.Write(pcal9555::kI2cAddress, write.data(), write.size()));
  const std::array<uint8_t, 1> reg{0x06U};
  std::array<uint8_t, 2> rx{};
  KB_CHECK(port.WriteRead(pcal9555::kI2cAddress, reg.data(), reg.size(), rx.data(), rx.size()));
  KB_CHECK_EQ(rx[0], 0xF0U);
  KB_CHECK_EQ(rx[1], 0x0FU);
  pcal9555::I2cFailure failure = pcal9555::I2cFailure::Other;
  KB_CHECK(!port.GetLastFailure(failure));
  KB_CHECK(!port.Write(0x21U, write.data(), write.size()));
  KB_CHECK(port.GetLastFailure(failure) && (failure == pcal9555::I2cFailure::AddressNack));
  bus.FailNext(i::I2cFailure::DataNack);
  KB_CHECK(!port.Write(pcal9555::kI2cAddress, write.data(), write.size()));
  KB_CHECK(port.GetLastFailure(failure) && (failure == pcal9555::I2cFailure::DataNack));
  bus.FailNext(i::I2cFailure::BusStuck);
  KB_CHECK(!port.Write(pcal9555::kI2cAddress, write.data(), write.size()));
  KB_CHECK(port.GetLastFailure(failure) && (failure == pcal9555::I2cFailure::Other));
  port.Log(pcal9555::LogLevel::Error, "e");
  port.Log(pcal9555::LogLevel::Warning, "w");
  port.Log(pcal9555::LogLevel::Info, "i");
  port.Log(pcal9555::LogLevel::Debug, "d");
  KB_CHECK_EQ(log.Count(i::LogLevel::Error), 1U);
  KB_CHECK_EQ(log.Count(i::LogLevel::Debug), 1U);
  KB_CHECK(log.Last() == "d");
}
