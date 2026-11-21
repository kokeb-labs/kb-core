// Thin adapters from kb::iface contracts to three drivers' own port concepts, the shape a
// kb::handlers binding takes (M4): each adapter derives from the driver's CRTP port base and
// forwards each hook to a kb contract in a line or two. Compiled against the pinned drivers
// (components/drivers/driver-*), with host fakes and with *Ref values. Test code.
// SPDX-License-Identifier: MIT Copyright (c) 2026 The kb-core authors
#ifndef KB_INTERFACES_TEST_DRIVER_ADAPTERS_HPP
#define KB_INTERFACES_TEST_DRIVER_ADAPTERS_HPP

#include <cstddef>
#include <cstdint>
#include <span>

#include "bno08x_interface.hpp"
#include "kb/interfaces/interfaces.hpp"
#include "kb/utils/time.hpp"
#include "pcal9555_i2c_interface.hpp"
#include "tmc5160_spi_interface.hpp"
#include "tmc5160_uart_interface.hpp"

namespace adapters {

inline bool Ok(kb::Status s) noexcept {
  return s == kb::Status::Ok;
}

// --- tmc5160: SPI (datagram) and single-wire UART (Exchange)
// -------------------------------------------------
template <kb::iface::SpiDevice Dev>
class Tmc5160Spi : public tmc5160::SpiInterface<Tmc5160Spi<Dev>> {
public:
  explicit Tmc5160Spi(Dev& dev) noexcept : dev_(dev) {}
  bool EnsureInitialized() noexcept {
    return Ok(dev_.EnsureReady());
  }
  bool Transfer(const uint8_t* tx, uint8_t* rx, std::size_t len) noexcept {
    return Ok(dev_.Transfer(std::span<const uint8_t>{tx, len}, std::span<uint8_t>{rx, len}));
  }

private:
  Dev& dev_;
};

template <kb::iface::UartPort Uart>
class Tmc5160Uart : public tmc5160::UartInterface<Tmc5160Uart<Uart>> {
public:
  explicit Tmc5160Uart(Uart& uart) noexcept : uart_(uart) {}
  bool EnsureInitialized() noexcept {
    return Ok(uart_.EnsureReady());
  }
  tmc5160::BusStatus Exchange(const uint8_t* tx, std::size_t tx_len, uint8_t* rx,
                              std::size_t rx_len, uint32_t timeout_ms) noexcept {
    tmc5160::BusStatus status = tmc5160::BusStatus::Fault;
    if (Ok(uart_.FlushInput()) && Ok(uart_.Write(std::span<const uint8_t>{tx, tx_len}))) {
      const kb::Result<std::size_t> got =
          uart_.Read(std::span<uint8_t>{rx, rx_len}, kb::Milliseconds{timeout_ms});
      status = (got.IsOk() && (got.Value() == rx_len)) ? tmc5160::BusStatus::Ok
                                                       : tmc5160::BusStatus::Timeout;
    }
    return status;
  }

private:
  Uart& uart_;
};

// --- bno08x: SHTP over I2C (with H_INTN) and over SPI (with WAKE)
// -------------------------------------------
template <kb::iface::I2cBus Bus, kb::iface::GpioInput Int, kb::iface::MonotonicClock Clock>
class Bno08xI2c : public bno08x::HubInterface<Bno08xI2c<Bus, Int, Clock>> {
public:
  static constexpr bno08x::Transport kTransport = bno08x::Transport::I2c;
  Bno08xI2c(Bus& bus, Int& int_pin, const Clock& clock) noexcept
      : bus_(bus), int_(int_pin), clock_(clock) {}
  bool EnsureInitialized() noexcept {
    return Ok(bus_.EnsureReady());
  }
  uint32_t NowUs() noexcept {
    return kb::utils::NarrowUs(clock_.Now());
  }
  bool I2cWrite(uint8_t address, const uint8_t* data, std::size_t len) noexcept {
    return Ok(bus_.Write(address, std::span<const uint8_t>{data, len}));
  }
  bool I2cRead(uint8_t address, uint8_t* data, std::size_t len) noexcept {
    return Ok(bus_.Read(address, std::span<uint8_t>{data, len}));
  }
  bool IntAsserted() noexcept {
    kb::iface::Level level = kb::iface::Level::High;
    return Ok(int_.Get(level)) && (level == kb::iface::Level::Low);
  }

private:
  Bus& bus_;
  Int& int_;
  const Clock& clock_;
};

template <kb::iface::SpiDevice Dev, kb::iface::GpioOutput Wake, kb::iface::GpioInput Int,
          kb::iface::MonotonicClock Clock>
class Bno08xSpi : public bno08x::HubInterface<Bno08xSpi<Dev, Wake, Int, Clock>> {
public:
  static constexpr bno08x::Transport kTransport = bno08x::Transport::Spi;
  Bno08xSpi(Dev& dev, Wake& wake, Int& int_pin, const Clock& clock) noexcept
      : dev_(dev), wake_(wake), int_(int_pin), clock_(clock) {}
  bool EnsureInitialized() noexcept {
    return Ok(dev_.EnsureReady());
  }
  uint32_t NowUs() noexcept {
    return kb::utils::NarrowUs(clock_.Now());
  }
  bool SpiTransfer(const uint8_t* tx, uint8_t* rx, std::size_t len) noexcept {
    return Ok(dev_.Transfer(std::span<const uint8_t>{tx, len}, std::span<uint8_t>{rx, len}));
  }
  bool SetWake(bool asserted) noexcept {
    return Ok(wake_.Set(asserted ? kb::iface::Level::Low : kb::iface::Level::High));
  }
  bool IntAsserted() noexcept {
    kb::iface::Level level = kb::iface::Level::High;
    return Ok(int_.Get(level)) && (level == kb::iface::Level::Low);
  }

private:
  Dev& dev_;
  Wake& wake_;
  Int& int_;
  const Clock& clock_;
};

// --- pcal9555: I2C register bus, with the optional failure and log hooks
// ----------------------------------------
template <kb::iface::I2cBus Bus, kb::iface::LogSink Sink>
class Pcal9555I2c : public pcal9555::I2cInterface<Pcal9555I2c<Bus, Sink>> {
public:
  Pcal9555I2c(Bus& bus, Sink& log) noexcept : bus_(bus), log_(log) {}
  bool EnsureInitialized() noexcept {
    return Ok(bus_.EnsureReady());
  }
  bool Write(uint8_t address, const uint8_t* data, std::size_t len) noexcept {
    return Ok(bus_.Write(address, std::span<const uint8_t>{data, len}));
  }
  bool WriteRead(uint8_t address, const uint8_t* tx, std::size_t tx_len, uint8_t* rx,
                 std::size_t rx_len) noexcept {
    return Ok(bus_.WriteRead(address, std::span<const uint8_t>{tx, tx_len},
                             std::span<uint8_t>{rx, rx_len}));
  }
  bool GetLastFailure(pcal9555::I2cFailure& failure) noexcept {
    const kb::iface::I2cFailure f = bus_.LastFailure();
    switch (f) {
    case kb::iface::I2cFailure::AddressNack:
      failure = pcal9555::I2cFailure::AddressNack;
      break;
    case kb::iface::I2cFailure::DataNack:
      failure = pcal9555::I2cFailure::DataNack;
      break;
    case kb::iface::I2cFailure::None:
    case kb::iface::I2cFailure::Bus:
    case kb::iface::I2cFailure::Timeout:
    case kb::iface::I2cFailure::BusStuck:
    default:
      failure = pcal9555::I2cFailure::Other;
      break;
    }
    return f != kb::iface::I2cFailure::None;
  }
  void Log(pcal9555::LogLevel level, const char* message) noexcept {
    kb::iface::LogLevel mapped = kb::iface::LogLevel::Debug;
    switch (level) {
    case pcal9555::LogLevel::Error:
      mapped = kb::iface::LogLevel::Error;
      break;
    case pcal9555::LogLevel::Warning:
      mapped = kb::iface::LogLevel::Warning;
      break;
    case pcal9555::LogLevel::Info:
      mapped = kb::iface::LogLevel::Info;
      break;
    case pcal9555::LogLevel::Debug:
    default:
      mapped = kb::iface::LogLevel::Debug;
      break;
    }
    log_.Write(mapped, std::string_view{message});
  }

private:
  Bus& bus_;
  Sink& log_;
};

} // namespace adapters

#endif
