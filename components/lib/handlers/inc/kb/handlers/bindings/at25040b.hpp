/**
 * @file at25040b.hpp
 * @brief The at25040b binding and handler: AT25010B / AT25020B / AT25040B SPI serial EEPROM
 * (128 to 512 bytes, 8-byte pages, BP1:BP0 block protection, WP pin).
 *
 * | Piece | What |
 * | --- | --- |
 * | Port | `Port<Dev>`: the driver's SPI hooks over a kb::iface::SpiDevice (SpiHooks, one chip
 * select), the delay of the write-cycle polls, and the optional WP pin (`GpioSet`, GpioHooks; no
 * pin = WP tied high) | | Errors | `ToStatus()`: every `at25040b::Error` to a kb::Status; the code
 * in the health record's detail | | Start-up | `Identify + Configure` =
 * `driver.EnsureInitialized()` (sync): configuration check, WP parked, idle status read twice,
 * write-enable latch toggle (or a stable head read) | | Thread model | Guarded (`kRequiresOwner =
 * false`): a slow memory used now and then from several tasks; nothing to sample | | Health |
 * `NotInitialized` faults at once; bus failures, timeouts, protection refusals and write mismatches
 * count to the threshold | | App API | no snapshot; `Read()` and `Write()` of any length (one
 * request per 16-byte chunk), `ReadStatus()`, `GetBlockProtection()`, `SetBlockProtection()` |
 *
 * Each request moves at most `kChunkBytes`; `Read()` and `Write()` split a longer range into
 * chunks, each one request, all within the one `timeout`. A failed chunk ends the call; the chunks
 * before it are done.
 *
 * @code
 * kb::handlers::BusDomain<Dev, kb::handlers::ThreadModel::Guarded> spi2{eeprom_cs};
 * kb::handlers::Guarded<kb::handlers::at25040b::Handler<Dev>, decltype(spi2)> rom{spi2, eeprom_cs,
 * timing, cfg}; std::array<uint8_t, 32> blob{}; if
 * (rom.EnsureStarted(kb::Milliseconds{20U}).IsOk()) {
 *   static_cast<void>(kb::handlers::at25040b::Read(rom, 0x0100U, blob, kb::Milliseconds{20U}));
 * }
 * @endcode
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_HANDLERS_BINDINGS_AT25040B_HPP
#define KB_HANDLERS_BINDINGS_AT25040B_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "kb/handlers/handlers.hpp"
#include <at25040b.hpp>

namespace kb::handlers::at25040b {

/** @brief The one map of the driver's errors to kb::Status (every enumerator, no default). */
[[nodiscard]] constexpr kb::Status ToStatus(::at25040b::Error error) noexcept {
  kb::Status s = kb::Status::Fault;
  switch (error) {
  case ::at25040b::Error::Ok:
    s = kb::Status::Ok;
    break;
  case ::at25040b::Error::NotInitialized:
    s = kb::Status::NotReady;
    break;
  case ::at25040b::Error::InvalidArgument:
  case ::at25040b::Error::BlockProtected:
    s = kb::Status::InvalidArgument;
    break;
  case ::at25040b::Error::Bus:
  case ::at25040b::Error::BusNoise:
  case ::at25040b::Error::Pin:
    s = kb::Status::BusError;
    break;
  case ::at25040b::Error::Timeout:
    s = kb::Status::Timeout;
    break;
  case ::at25040b::Error::DeviceIdMismatch:
  case ::at25040b::Error::DeviceFault:
  case ::at25040b::Error::WriteProtected:
  case ::at25040b::Error::WriteMismatch:
    s = kb::Status::Fault;
    break;
  case ::at25040b::Error::Crc:
    s = kb::Status::CrcMismatch;
    break;
  case ::at25040b::Error::Busy:
    s = kb::Status::Busy;
    break;
  case ::at25040b::Error::Unsupported:
    s = kb::Status::Unsupported;
    break;
  case ::at25040b::Error::NoDevice:
    s = kb::Status::NoDevice;
    break;
  }
  return s;
}

/** @brief The control output of the port: WP (asserted = writes blocked; the pin is active low). */
inline constexpr std::size_t kWriteProtectPin = 0U;

/** @brief Bytes one request moves: one READ chunk of the driver. */
inline constexpr std::size_t kChunkBytes = ::at25040b::kReadChunkBytes;

/**
 * @brief The driver's SPI port over a kb::iface::SpiDevice and the optional WP pin.
 * @tparam Dev The device (a SpiDevice or SpiDeviceRef).
 */
template <kb::iface::SpiDevice Dev>
class Port : public ::at25040b::SpiInterface<Port<Dev>>,
             public SpiHooks<Port<Dev>, Dev>,
             public GpioHooks<Port<Dev>, 1U, 0U> {
  using Bus = SpiHooks<Port<Dev>, Dev>;
  using Pins = GpioHooks<Port<Dev>, 1U, 0U>;

public:
  /** @brief Over `dev` with `timing` and the WP pin `wp` (absent: tied high); no I/O. */
  Port(Dev& dev, const HookTiming& timing, const OutputPin& wp) noexcept
      : Bus(dev, timing), Pins(std::array<OutputPin, 1U>{wp}, std::array<InputPin, 0U>{}) {}

  /** @brief Hook: bring the device up. */
  bool EnsureInitialized() noexcept {
    return this->BusReady();
  }
  /** @brief Hook: one full-duplex transfer with the chip select held. */
  bool Transfer(const uint8_t* tx, uint8_t* rx, std::size_t len) noexcept {
    return this->BusTransfer(tx, rx, len);
  }
  /** @brief Hook: wait at least `us` microseconds (write-cycle polls). */
  void DelayUs(uint32_t us) noexcept {
    this->Timing().DelayUs(us);
  }
  /** @brief Optional hook: WP; true without a pin (WP tied high, writes allowed by the pin). */
  bool GpioSet(::at25040b::CtrlPin pin, ::at25040b::GpioSignal signal) noexcept {
    static_cast<void>(pin);
    bool ok = true;
    if (this->HasOutput(kWriteProtectPin)) {
      ok = this->DriveOutput(kWriteProtectPin, signal == ::at25040b::GpioSignal::ACTIVE);
    }
    return ok;
  }

  /** @brief The transfer counters. */
  [[nodiscard]] const HookCounters& BusCounters() const noexcept {
    return Bus::Counters();
  }
  /** @brief The pin counters. */
  [[nodiscard]] const HookCounters& PinCounters() const noexcept {
    return Pins::Counters();
  }
};

/** @brief The driver type a handler over `Dev` owns. */
template <kb::iface::SpiDevice Dev>
using Driver = ::at25040b::AT25040B<Port<Dev>>;

/** @brief The handler's configuration: board values from the HAL. */
struct Config {
  ::at25040b::Config chip{};            ///< Variant, write-enable check, write timeout.
  HandlerConfig handler{};              ///< Health, recovery, polling.
  kb::Milliseconds start_deadline{20U}; ///< Bound of the start-up stage.
  OutputPin write_protect{{},
                          true}; ///< WP; active low (asserted = pin low); absent when tied high.
};

/** @brief The commands of the handler. */
enum class Op : uint8_t {
  Read = 0,               ///< `length` bytes from `address` into the reply.
  Write = 1,              ///< `length` bytes of `data` to `address` (pages split by the driver).
  ReadStatus = 2,         ///< The status register.
  GetBlockProtection = 3, ///< BP1:BP0 read from the chip.
  SetBlockProtection = 4, ///< BP1:BP0 to `protect`.
};

/** @brief One command. */
struct Command {
  std::array<uint8_t, kChunkBytes> data{};             ///< Write: the bytes.
  uint16_t address{0U};                                ///< Read, Write: the first byte.
  uint8_t length{0U};                                  ///< Read, Write: 1 to kChunkBytes.
  Op op{Op::ReadStatus};                               ///< What to do.
  ::at25040b::Verify verify{::at25040b::Verify::None}; ///< Read twice and compare.
  ::at25040b::BlockProtect protect{::at25040b::BlockProtect::None}; ///< SetBlockProtection.
};

/** @brief The reply of a command. */
struct Reply {
  std::array<uint8_t, kChunkBytes> data{}; ///< Read: the bytes.
  uint8_t length{0U};                      ///< Read: bytes in `data`.
  uint8_t status_raw{0U};                  ///< ReadStatus: the byte.
  bool write_enabled{false};               ///< ReadStatus: WEN.
  ::at25040b::BlockProtect protect{
      ::at25040b::BlockProtect::None}; ///< Status and protection reads.
};

/**
 * @brief The at25040b handler.
 * @tparam Dev The device (a SpiDevice or SpiDeviceRef).
 */
template <kb::iface::SpiDevice Dev>
class Handler : public HandlerCore<Handler<Dev>, Driver<Dev>, NoSnapshot, Command, Reply> {
  using Core = HandlerCore<Handler<Dev>, Driver<Dev>, NoSnapshot, Command, Reply>;

public:
  /// The start-up plan: one sync stage, the driver's EnsureInitialized() (ADR-0010 generic plan).
  /// The chip has no reset or enable pin; the driver drives WP itself.
  static constexpr std::array<Stage, 1> kStartupPlan{
      {{static_cast<uint16_t>(stage::kIdentify | stage::kConfigure), StageMode::Sync, false}}};
  static constexpr bool kRequiresOwner = false; ///< Served by occasional calls.
  static constexpr BusKind kBus = BusKind::Spi; ///< On an SPI chip select.

  /** @brief Stores references and the configuration; no I/O. */
  Handler(Dev& dev, const HookTiming& timing, const Config& config) noexcept
      : Core(config.handler), port_(dev, timing, config.write_protect), config_(config) {}

  /// @name Binding hooks (called by HandlerCore)
  /// @{
  void EmplaceDriver(std::optional<Driver<Dev>>& slot) noexcept {
    slot.emplace(port_, config_.chip);
  }
  [[nodiscard]] kb::Result<StartProgress> RunStage(const StageCall& call,
                                                   Driver<Dev>& driver) noexcept {
    static_cast<void>(call);
    const kb::Result<void> r = LiftStatus(driver.EnsureInitialized(), &ToStatus);
    return r.IsOk() ? kb::Result<StartProgress>::Success(StartProgress::Done)
                    : Forward<StartProgress>(r);
  }
  [[nodiscard]] kb::Milliseconds StageDeadline(std::size_t index) const noexcept {
    static_cast<void>(index);
    return config_.start_deadline;
  }
  [[nodiscard]] kb::Result<void> StopDriver(Driver<Dev>& driver) noexcept {
    return LiftStatus(driver.Deinitialize(), &ToStatus);
  }
  [[nodiscard]] bool IsFatal(kb::Status status, uint32_t detail) const noexcept {
    static_cast<void>(status);
    return detail == DetailOf(::at25040b::Error::NotInitialized);
  }
  [[nodiscard]] kb::Result<Reply> Run(Driver<Dev>& driver, const Command& c,
                                      kb::Instant now) noexcept {
    static_cast<void>(now);
    kb::Result<Reply> result = kb::Result<Reply>::Failure(kb::Status::InvalidArgument);
    switch (c.op) {
    case Op::Read:
      result = read(driver, c);
      break;
    case Op::Write:
      result = writeChunk(driver, c);
      break;
    case Op::ReadStatus:
      result = status(Lift(driver.ReadStatus(), &ToStatus));
      break;
    case Op::GetBlockProtection:
      result = protection(Lift(driver.GetBlockProtection(), &ToStatus));
      break;
    case Op::SetBlockProtection:
      result = done(LiftStatus(driver.SetBlockProtection(c.protect), &ToStatus));
      break;
    }
    return result;
  }
  /// @}

  /** @brief The port's transfer counters. */
  [[nodiscard]] const HookCounters& PortCounters() const noexcept {
    return port_.BusCounters();
  }

private:
  [[nodiscard]] static bool lengthOk(const Command& c) noexcept {
    return (c.length != 0U) && (static_cast<std::size_t>(c.length) <= kChunkBytes);
  }
  [[nodiscard]] static kb::Result<Reply> read(Driver<Dev>& driver, const Command& c) noexcept {
    kb::Result<Reply> result = kb::Result<Reply>::Failure(kb::Status::InvalidArgument);
    if (lengthOk(c)) {
      Reply reply{};
      const std::span<uint8_t> out = std::span<uint8_t>{reply.data}.first(c.length);
      const kb::Result<void> r = LiftStatus(driver.Read(c.address, out, c.verify), &ToStatus);
      reply.length = c.length;
      result = r.IsOk() ? kb::Result<Reply>::Success(reply) : Forward<Reply>(r);
    }
    return result;
  }
  [[nodiscard]] static kb::Result<Reply> writeChunk(Driver<Dev>& driver,
                                                    const Command& c) noexcept {
    kb::Result<Reply> result = kb::Result<Reply>::Failure(kb::Status::InvalidArgument);
    if (lengthOk(c)) {
      const std::span<const uint8_t> in = std::span<const uint8_t>{c.data}.first(c.length);
      result = done(LiftStatus(driver.Write(c.address, in, c.verify), &ToStatus));
    }
    return result;
  }
  [[nodiscard]] static kb::Result<Reply> done(const kb::Result<void>& r) noexcept {
    return r.IsOk() ? kb::Result<Reply>::Success(Reply{}) : Forward<Reply>(r);
  }
  [[nodiscard]] static kb::Result<Reply> status(const kb::Result<::at25040b::Status>& r) noexcept {
    kb::Result<Reply> result = Forward<Reply>(r);
    if (r.IsOk()) {
      Reply reply{};
      reply.status_raw = r.Value().raw;
      reply.write_enabled = r.Value().write_enabled;
      reply.protect = r.Value().block_protect;
      result = kb::Result<Reply>::Success(reply);
    }
    return result;
  }
  [[nodiscard]] static kb::Result<Reply> protection(
      const kb::Result<::at25040b::BlockProtect>& r) noexcept {
    kb::Result<Reply> result = Forward<Reply>(r);
    if (r.IsOk()) {
      Reply reply{};
      reply.protect = r.Value();
      result = kb::Result<Reply>::Success(reply);
    }
    return result;
  }

  Port<Dev> port_;
  Config config_;
};

/// @name App API: the same calls through OwnerAccess or Guarded
/// @{

/** @brief The size of the next chunk of a range of `total` bytes after `done` of them. */
[[nodiscard]] constexpr std::size_t ChunkAt(std::size_t total, std::size_t done) noexcept {
  const std::size_t left = total - done;
  return (left < kChunkBytes) ? left : kChunkBytes;
}

/** @brief The address `offset` bytes after `address` (ranges are checked by the driver). */
[[nodiscard]] constexpr uint16_t AddressAt(uint16_t address, std::size_t offset) noexcept {
  return static_cast<uint16_t>(static_cast<std::size_t>(address) + offset);
}

/** @brief `out.size()` bytes from `address`, one request per chunk, all within `timeout`. */
template <typename Access>
[[nodiscard]] kb::Result<void> Read(Access& io, uint16_t address, std::span<uint8_t> out,
                                    kb::Milliseconds timeout,
                                    ::at25040b::Verify verify = ::at25040b::Verify::None) noexcept {
  const kb::Instant begin = kb::rtos::Now();
  kb::Result<void> result = out.empty() ? kb::Result<void>::Failure(kb::Status::InvalidArgument)
                                        : kb::Result<void>::Success();
  std::size_t offset = 0U;
  while (result.IsOk() && (offset < out.size())) {
    Command c{};
    c.op = Op::Read;
    c.address = AddressAt(address, offset);
    c.length = static_cast<uint8_t>(ChunkAt(out.size(), offset));
    c.verify = verify;
    const kb::Result<Reply> r =
        io.Request(c, Remaining(timeout, kb::utils::ElapsedUs(kb::rtos::Now(), begin)));
    if (r.IsOk()) {
      const std::span<uint8_t> dst = out.subspan(offset, c.length);
      for (std::size_t i = 0U; i < dst.size(); ++i) {
        dst[i] = r.Value().data[i];
      }
      offset += c.length;
    } else {
      result = Forward<void>(r);
    }
  }
  return result;
}

/** @brief `data` to `address`, one request per chunk, all within `timeout`. */
template <typename Access>
[[nodiscard]] kb::Result<void> Write(Access& io, uint16_t address, std::span<const uint8_t> data,
                                     kb::Milliseconds timeout) noexcept {
  const kb::Instant begin = kb::rtos::Now();
  kb::Result<void> result = data.empty() ? kb::Result<void>::Failure(kb::Status::InvalidArgument)
                                         : kb::Result<void>::Success();
  std::size_t offset = 0U;
  while (result.IsOk() && (offset < data.size())) {
    Command c{};
    c.op = Op::Write;
    c.address = AddressAt(address, offset);
    c.length = static_cast<uint8_t>(ChunkAt(data.size(), offset));
    const std::span<const uint8_t> src = data.subspan(offset, c.length);
    for (std::size_t i = 0U; i < src.size(); ++i) {
      c.data[i] = src[i];
    }
    const kb::Result<Reply> r =
        io.Request(c, Remaining(timeout, kb::utils::ElapsedUs(kb::rtos::Now(), begin)));
    if (r.IsOk()) {
      offset += c.length;
    } else {
      result = Forward<void>(r);
    }
  }
  return result;
}

/** @brief The status register. */
template <typename Access>
[[nodiscard]] kb::Result<::at25040b::Status> ReadStatus(Access& io,
                                                        kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::ReadStatus;
  const kb::Result<Reply> r = io.Request(c, timeout);
  kb::Result<::at25040b::Status> result = Forward<::at25040b::Status>(r);
  if (r.IsOk()) {
    ::at25040b::Status s{};
    s.raw = r.Value().status_raw;
    s.write_enabled = r.Value().write_enabled;
    s.block_protect = r.Value().protect;
    result = kb::Result<::at25040b::Status>::Success(s);
  }
  return result;
}

/** @brief BP1:BP0 as the chip holds them. */
template <typename Access>
[[nodiscard]] kb::Result<::at25040b::BlockProtect> GetBlockProtection(
    Access& io, kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::GetBlockProtection;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<::at25040b::BlockProtect>::Success(r.Value().protect)
                  : Forward<::at25040b::BlockProtect>(r);
}

/** @brief BP1:BP0 to `level` (a non-volatile write cycle). */
template <typename Access>
[[nodiscard]] kb::Result<void> SetBlockProtection(Access& io, ::at25040b::BlockProtect level,
                                                  kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::SetBlockProtection;
  c.protect = level;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<void>::Success() : Forward<void>(r);
}

/// @}

} // namespace kb::handlers::at25040b

#endif // KB_HANDLERS_BINDINGS_AT25040B_HPP
